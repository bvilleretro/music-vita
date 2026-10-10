#pragma once
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/audioout.h>
#include <stdint.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"
#define DR_WAV_IMPLEMENTATION
#include "dr_wav.h"

#define OUT_RATE          48000   /* the Vita's native rate: everything is resampled to this */
#define OUT_GRAIN         2048    /* frames per audio output call (multiple of 64) */
#define OUT_NBUF          4       /* rotating output buffers */
#define DEC_CHUNK         2048    /* frames decoded per step */
#define RING_FRAMES       262144  /* power of two, ~5.4 s */
#define RING_MASK         (RING_FRAMES - 1)
#define TARGET_FRAMES     196608  /* decoder keeps ~4 s buffered ahead */
#define PREBUFFER_FRAMES  48000   /* ~1 s buffered before a new track starts */
#define SEEK_PREBUF       9600    /* ~0.2 s buffered after a seek (quick skimming) */
#define SEEK_POINTS       400     /* MP3 seek table size */
#define NBANDS            24
#define WAVE_PTS          160

static int has_ext(const char *name, const char *ext) {
    size_t n = strlen(name), e = strlen(ext);
    if (n < e) return 0;
    for (size_t i = 0; i < e; i++)
        if (tolower((unsigned char)name[n - e + i]) != ext[i]) return 0;
    return 1;
}

/* ---- shared state ---- */
static int16_t g_ring[RING_FRAMES * 2];
static volatile unsigned int g_wpos = 0, g_rpos = 0;
static volatile int g_run = 1;
static volatile int g_load_req = 0;
static char g_load_path[600];
static volatile int g_flush_req = 0;
static volatile int g_audio_ok = 0;
static volatile int g_paused = 0;
static volatile int g_loaded = 0;
static volatile int g_dec_done = 0;
static volatile int g_ended = 0;
static volatile int g_error = 0;
static volatile int g_volume = 80;
static volatile int g_seek_req = -1;            /* absolute target in ms, -1 = none */
static volatile unsigned int g_base_ms = 0;     /* position the output thread restarts from */
static volatile unsigned int g_pos_ms = 0;      /* current playback position */
static volatile unsigned int g_dur_ms = 0;      /* track length (0 = unknown yet) */
static volatile unsigned int g_prebuf = PREBUFFER_FRAMES;
static volatile unsigned int g_seconds = 0;
static volatile unsigned int g_underruns = 0;
static volatile int g_dec_x10 = 0;              /* decode speed x10 (e.g. 152 = 15.2x realtime) */
static volatile float g_vis_bass = 0.0f, g_vis_level = 0.0f;
static volatile float g_bands[NBANDS];
static volatile float g_wave[WAVE_PTS];

/* background scan (MP3 length + seek table) */
static volatile int g_track_serial = 0;         /* bumps on every track load */
static volatile int g_scan_req = 0;             /* serial the scan thread should work on */
static volatile int g_scan_done = 0;            /* serial whose results are ready */
static volatile unsigned int g_scan_cnt = 0;
static char s_scan_path[600];
static drmp3 s_scan;
static drmp3_seek_point s_pts_new[SEEK_POINTS];

/* ---- decoder thread: file -> (resample) -> ring buffer ---- */
static drmp3 s_mp3;
static drwav s_wav;
static int s_open = 0, s_is_mp3 = 0, s_ch = 2, s_bound = 0;

static void dec_close(void) {
    if (!s_open) return;
    if (s_is_mp3) drmp3_uninit(&s_mp3); else drwav_uninit(&s_wav);
    s_open = 0;
}

/* low-priority thread: measures the MP3 length and builds a seek table on its own file handle */
static int scan_thread(SceSize args, void *argp) {
    int last = 0;
    while (g_run) {
        int req = g_scan_req;
        if (req == 0 || req == last) { sceKernelDelayThread(20000); continue; }
        last = req;
        char path[600];
        strncpy(path, s_scan_path, sizeof(path) - 1);
        path[sizeof(path) - 1] = 0;
        if (drmp3_init_file(&s_scan, path, NULL)) {
            drmp3_uint64 pcm = drmp3_get_pcm_frame_count(&s_scan);
            drmp3_uint32 cnt = SEEK_POINTS;
            int ok = drmp3_calculate_seek_points(&s_scan, &cnt, s_pts_new) ? 1 : 0;
            unsigned int sr = s_scan.sampleRate;
            drmp3_uninit(&s_scan);
            if (sr > 0 && req == g_track_serial) {
                g_dur_ms = (unsigned int)(pcm * 1000ULL / (unsigned long long)sr);
                g_scan_cnt = ok ? (unsigned int)cnt : 0;
                __sync_synchronize();
                g_scan_done = req;
            }
        }
    }
    return 0;
}

static int decoder_thread(SceSize args, void *argp) {
    static int16_t tmp[DEC_CHUNK * 2];
    static float   fw[(DEC_CHUNK + 1) * 2];
    double ph = 0.0, step = 1.0;
    float prev_l = 0.0f, prev_r = 0.0f;
    int first = 1, rate = 44100;
    float speed_sm = 0.0f;

    while (g_run) {
        /* ---- load a new track ---- */
        if (g_load_req) {
            char path[600];
            strncpy(path, g_load_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = 0;
            g_load_req = 0;
            g_track_serial++;

            dec_close();
            g_loaded = 0; g_dec_done = 0; g_ended = 0; g_error = 0;
            g_paused = 0; g_seconds = 0; g_pos_ms = 0; g_dur_ms = 0;
            g_seek_req = -1;
            int r = 44100;

            if (has_ext(path, ".mp3")) {
                if (drmp3_init_file(&s_mp3, path, NULL)) {
                    s_is_mp3 = 1; s_ch = s_mp3.channels; r = s_mp3.sampleRate; s_open = 1;
                }
            } else {
                if (drwav_init_file(&s_wav, path, NULL)) {
                    s_is_mp3 = 0; s_ch = s_wav.channels; r = s_wav.sampleRate; s_open = 1;
                }
            }
            if (s_open && (s_ch < 1 || s_ch > 2 || r < 8000 || r > 192000)) dec_close();
            if (s_open && !g_audio_ok) dec_close();

            if (s_open) {
                rate = r;
                step = (double)rate / (double)OUT_RATE;
                s_bound = 0;
                if (s_is_mp3) {
                    /* length + seek table come from the background scan thread */
                    strncpy(s_scan_path, path, sizeof(s_scan_path) - 1);
                    s_scan_path[sizeof(s_scan_path) - 1] = 0;
                    __sync_synchronize();
                    g_scan_req = g_track_serial;
                } else {
                    g_dur_ms = (unsigned int)((unsigned long long)s_wav.totalPCMFrameCount * 1000ULL
                                              / (unsigned long long)rate);
                }

                ph = 0.0; prev_l = prev_r = 0.0f; first = 1;
                g_base_ms = 0;
                g_prebuf = PREBUFFER_FRAMES;
                __sync_synchronize();
                g_flush_req = 1;
                while (g_flush_req && g_run) sceKernelDelayThread(1000);
                g_loaded = 1;
            } else {
                g_error = 1;
            }
        }

        /* ---- scan finished: give the decoder its seek table ---- */
        if (s_open && s_is_mp3 && !s_bound && g_scan_done == g_track_serial) {
            if (g_scan_cnt > 0) drmp3_bind_seek_table(&s_mp3, g_scan_cnt, s_pts_new);
            s_bound = 1;
        }

        /* ---- seek request (skimming) ---- */
        if (g_seek_req >= 0) {
            int ms = g_seek_req;
            g_seek_req = -1;
            if (s_open && g_loaded) {
                if (g_dur_ms > 600 && ms > (int)g_dur_ms - 500) ms = (int)g_dur_ms - 500;
                if (ms < 0) ms = 0;
                unsigned long long tf = (unsigned long long)ms * (unsigned long long)rate / 1000ULL;
                if (s_is_mp3) drmp3_seek_to_pcm_frame(&s_mp3, tf);
                else          drwav_seek_to_pcm_frame(&s_wav, tf);
                ph = 0.0; prev_l = prev_r = 0.0f; first = 1;
                g_dec_done = 0;
                g_base_ms = (unsigned int)ms;
                g_prebuf = SEEK_PREBUF;
                __sync_synchronize();
                g_flush_req = 1;
                while (g_flush_req && g_run) sceKernelDelayThread(1000);
            }
        }

        /* ---- decode ahead ---- */
        if (s_open && !g_dec_done) {
            if ((g_wpos - g_rpos) < TARGET_FRAMES) {
                SceUInt64 t0 = sceKernelGetProcessTimeWide();
                unsigned long long got;
                if (s_is_mp3) got = drmp3_read_pcm_frames_s16(&s_mp3, DEC_CHUNK, tmp);
                else          got = drwav_read_pcm_frames_s16(&s_wav, DEC_CHUNK, tmp);

                unsigned int w = g_wpos;
                unsigned int outn = 0;

                if (rate == OUT_RATE) {
                    for (unsigned int i = 0; i < (unsigned int)got; i++) {
                        int l = tmp[i * s_ch];
                        int rr = (s_ch == 2) ? tmp[i * 2 + 1] : l;
                        unsigned int idx = (w + i) & RING_MASK;
                        g_ring[idx * 2]     = (int16_t)l;
                        g_ring[idx * 2 + 1] = (int16_t)rr;
                    }
                    outn = (unsigned int)got;
                } else if (got > 0) {
                    fw[0] = prev_l; fw[1] = prev_r;
                    for (unsigned int i = 0; i < (unsigned int)got; i++) {
                        int l = tmp[i * s_ch];
                        int rr = (s_ch == 2) ? tmp[i * 2 + 1] : l;
                        fw[(i + 1) * 2]     = (float)l;
                        fw[(i + 1) * 2 + 1] = (float)rr;
                    }
                    if (first) { fw[0] = fw[2]; fw[1] = fw[3]; first = 0; }
                    unsigned int o = 0;
                    double lim = (double)got;
                    while (ph < lim) {
                        int i = (int)ph;
                        float f = (float)(ph - (double)i);
                        float al = fw[i * 2],     ar = fw[i * 2 + 1];
                        float bl = fw[(i + 1) * 2], br = fw[(i + 1) * 2 + 1];
                        unsigned int idx = (w + o) & RING_MASK;
                        g_ring[idx * 2]     = (int16_t)(al + (bl - al) * f);
                        g_ring[idx * 2 + 1] = (int16_t)(ar + (br - ar) * f);
                        ph += step;
                        o++;
                    }
                    ph -= lim;
                    prev_l = fw[got * 2];
                    prev_r = fw[got * 2 + 1];
                    outn = o;
                }

                SceUInt64 dt = sceKernelGetProcessTimeWide() - t0;
                if (dt > 0 && got > 0) {
                    float sp = ((float)got / (float)rate) / ((float)dt / 1000000.0f);
                    speed_sm = (speed_sm == 0.0f) ? sp : speed_sm * 0.9f + sp * 0.1f;
                    g_dec_x10 = (int)(speed_sm * 10.0f);
                }

                __sync_synchronize();
                g_wpos = w + outn;
                /* the file stays open at the end so seeking back still works */
                if (got < DEC_CHUNK) g_dec_done = 1;
            } else {
                sceKernelDelayThread(4000);
            }
        } else {
            sceKernelDelayThread(8000);
        }
    }
    dec_close();
    return 0;
}

/* ---- output thread: ring buffer -> speakers (+ analysis for visualizers) ---- */
static int16_t g_obuf[OUT_NBUF][OUT_GRAIN * 2];
static int16_t g_zbuf[OUT_GRAIN * 2];     /* silence */

static void out_submit(int port, int16_t *buf) {
    int rc = sceAudioOutOutput(port, buf);
    if (rc < 0) sceKernelDelayThread(5000);   /* never spin if the call fails */
}

static int output_thread(SceSize args, void *argp) {
    static float xbuf[OUT_GRAIN];
    static float hann[OUT_GRAIN];
    float coef[NBANDS];
    unsigned long long played = 0;
    int started = 0, cur = 0;
    float lp = 0.0f;

    for (int i = 0; i < OUT_GRAIN; i++)
        hann[i] = 0.5f - 0.5f * cosf(6.2831853f * i / (OUT_GRAIN - 1));
    for (int b = 0; b < NBANDS; b++) {
        float f = 50.0f * powf(14000.0f / 50.0f, (float)b / (NBANDS - 1));
        coef[b] = 2.0f * cosf(6.2831853f * f / OUT_RATE);
    }

    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, OUT_GRAIN,
                                   OUT_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    g_audio_ok = (port >= 0);

    while (g_run) {
        if (g_flush_req) {
            g_rpos = g_wpos;
            started = 0;
            played = (unsigned long long)g_base_ms * OUT_RATE / 1000ULL;
            g_pos_ms = g_base_ms;
            g_seconds = g_base_ms / 1000;
            lp = 0.0f;
            __sync_synchronize();
            g_flush_req = 0;
            continue;
        }
        if (port < 0) { sceKernelDelayThread(10000); continue; }

        if (!g_loaded || g_paused) { out_submit(port, g_zbuf); continue; }

        unsigned int avail = g_wpos - g_rpos;

        if (g_dec_done && avail == 0) { g_loaded = 0; g_ended = 1; continue; }

        if (!started) {
            if (avail >= g_prebuf || (g_dec_done && avail > 0)) started = 1;
            else { out_submit(port, g_zbuf); continue; }
        }
        if (avail < OUT_GRAIN && !g_dec_done) {
            g_underruns++;
            started = 0;                    /* rebuffer (silence) instead of stuttering */
            out_submit(port, g_zbuf);
            continue;
        }

        unsigned int n = (avail >= OUT_GRAIN) ? OUT_GRAIN : avail;
        __sync_synchronize();
        unsigned int r0 = g_rpos;
        int vol = g_volume;
        int16_t *o = g_obuf[cur];
        float sumsq = 0.0f, bsq = 0.0f;
        for (unsigned int i = 0; i < n; i++) {
            unsigned int idx = (r0 + i) & RING_MASK;
            int l = g_ring[idx * 2], r = g_ring[idx * 2 + 1];
            float m = (l + r) * 0.5f;
            lp += 0.05f * (m - lp);
            sumsq += m * m;
            bsq += lp * lp;
            o[i * 2]     = (int16_t)((l * vol) / 100);
            o[i * 2 + 1] = (int16_t)((r * vol) / 100);
        }
        for (unsigned int i = n; i < OUT_GRAIN; i++) { o[i * 2] = 0; o[i * 2 + 1] = 0; }
        g_rpos = r0 + n;

        g_vis_level = sqrtf(sumsq / n) / 32768.0f;
        g_vis_bass  = sqrtf(bsq / n) / 32768.0f;
        played += n;
        g_pos_ms = (unsigned int)(played * 1000ULL / OUT_RATE);
        g_seconds = g_pos_ms / 1000;

        out_submit(port, o);
        cur = (cur + 1) % OUT_NBUF;

        /* spectrum + waveform analysis while the buffer plays */
        if (n == OUT_GRAIN) {
            for (int i = 0; i < OUT_GRAIN; i++) {
                unsigned int idx = (r0 + i) & RING_MASK;
                float m = (g_ring[idx * 2] + g_ring[idx * 2 + 1]) * (0.5f / 32768.0f);
                xbuf[i] = m * hann[i];
            }
            for (int b = 0; b < NBANDS; b++) {
                float c = coef[b], s1 = 0.0f, s2 = 0.0f;
                for (int j = 0; j < OUT_GRAIN; j++) {
                    float s0 = xbuf[j] + c * s1 - s2;
                    s2 = s1; s1 = s0;
                }
                float p = s1 * s1 + s2 * s2 - c * s1 * s2;
                float amp = sqrtf(p > 0.0f ? p : 0.0f) * (4.0f / OUT_GRAIN);
                float db = 20.0f * log10f(amp + 1e-6f);
                float v = (db + 66.0f) / 56.0f + b * 0.008f;
                if (v < 0.0f) v = 0.0f;
                if (v > 1.0f) v = 1.0f;
                g_bands[b] = v;
            }
            for (int p = 0; p < WAVE_PTS; p++) {
                unsigned int idx = (r0 + (unsigned int)(p * (OUT_GRAIN / WAVE_PTS))) & RING_MASK;
                g_wave[p] = (g_ring[idx * 2] + g_ring[idx * 2 + 1]) * (0.5f / 32768.0f);
            }
        }
    }
    if (port >= 0) sceAudioOutReleasePort(port);
    return 0;
}

static void audio_start(void) {
    SceUID t2 = sceKernelCreateThread("mv_output", output_thread, 0x40, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t2, 0, NULL);
    SceUID t1 = sceKernelCreateThread("mv_decode", decoder_thread, 0x60, 0x20000, 0, 0, NULL);
    sceKernelStartThread(t1, 0, NULL);
    SceUID t3 = sceKernelCreateThread("mv_scan", scan_thread, 0xB8, 0x10000, 0, 0, NULL);
    sceKernelStartThread(t3, 0, NULL);
}
