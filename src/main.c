#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <png.h>

#include "audio.h"
#include "meta.h"

/* Give the app a bigger malloc heap (cover art decoding needs it) */
int _newlib_heap_size_user = 64 * 1024 * 1024;

#define APP_VERSION "v1.0"
#define APP_AUTHOR  "by Bville Retro"
#define MUSIC_DIR   "ux0:data/MusicVita"
#define THEME_DIR   "ux0:data/MusicVita/theme"
#define PLAYLIST_DIR "ux0:data/MusicVita/playlists"
#define SETTINGS_FILE "ux0:data/MusicVita/settings.txt"

#define MAX_TRACKS  512
#define ART_TARGET  300       /* covers are decoded down to about this size */
#define MAX_TEX     24
#define LIST_ROWS   14
#define ROW_H       26
#define TILE_COLS   5
#define TILE_W      180
#define TILE_H      186
#define TILE_ART    140
#define MAX_SHAPES  64
#define MAX_PS      28
#define MAX_LASERS  24
#define VIS_COUNT   5
#define BAR_SEGS    16
#define FRAME_BUDGET  1500    /* max line/rectangle draws per frame (GPU safety limit) */
#define WAVE_W      1024
#define WAVE_H      128
#define MAX_PLAYLISTS 24
#define MAX_PL_SONGS  256

enum { TAB_LIST = 0, TAB_TILES, TAB_PLAYLISTS, TAB_SETTINGS, TAB_COUNT };
static const char *TAB_NAMES[TAB_COUNT] = { "List", "Tiles", "Playlists", "Settings" };
static const char *VIS_NAMES[VIS_COUNT] = { "Shapes", "Lasers", "Spectrum", "Symbols", "Ribbon" };

/* ---- per-frame draw budget: every line/rectangle goes through these ---- */
static int g_budget = 0;
static inline void b_line(float x0, float y0, float x1, float y1, unsigned c) {
    if (g_budget > 0) { g_budget--; vita2d_draw_line(x0, y0, x1, y1, c); }
}
static inline void b_rect(float x, float y, float w, float h, unsigned c) {
    if (g_budget > 0) { g_budget--; vita2d_draw_rectangle(x, y, w, h, c); }
}
#define vita2d_draw_line b_line
#define vita2d_draw_rectangle b_rect

/* ================= fonts (always whole-pixel sizes, never smaller than 12 px) ================= */
static vita2d_pgf  *g_pgf;
static vita2d_font *g_font = NULL;
#define FSZ 16.0f

static unsigned px(float s) {
    unsigned v = (unsigned)(s * FSZ + 0.5f);
    return v < 12 ? 12 : v;
}
static int text_w(float s, const char *t) {
    return g_font ? vita2d_font_text_width(g_font, px(s), t) : vita2d_pgf_text_width(g_pgf, s, t);
}
static int text_h(float s, const char *t) {
    return g_font ? vita2d_font_text_height(g_font, px(s), t) : vita2d_pgf_text_height(g_pgf, s, t);
}
static void draw_text(int x, int y, unsigned c, float s, const char *t) {
    if (g_font) vita2d_font_draw_text(g_font, x, y, c, px(s), t);
    else vita2d_pgf_draw_text(g_pgf, x, y, c, s, t);
}
static void draw_text_right(int xr, int y, unsigned c, float s, const char *t) {
    draw_text(xr - text_w(s, t), y, c, s, t);
}

/* ================= theme colors ================= */
typedef struct {
    unsigned bg, bg2, header, panel, select, accent, text, subtext;
    vita2d_texture *bgtex;
} Theme;
static Theme T;          /* the theme in use */
static Theme T_custom;   /* colors from theme.txt (+ background.png) */

/* the original blue palette; the premade themes are this palette with the hue shifted */
static const unsigned BASE[8] = {
    RGBA8(3, 7, 20, 255),        /* bg      */
    RGBA8(10, 31, 82, 255),      /* bg2     */
    RGBA8(8, 20, 51, 255),       /* header  */
    RGBA8(10, 22, 54, 255),      /* panel   */
    RGBA8(26, 60, 143, 255),     /* select  */
    RGBA8(79, 183, 255, 255),    /* accent  */
    RGBA8(232, 240, 255, 255),   /* text    */
    RGBA8(165, 185, 228, 255)    /* subtext */
};

typedef struct { const char *name; float hue; float sat; } Preset;
#define NPRESETS 8
static const Preset PRESETS[NPRESETS] = {
    { "Midnight Blue", 0.0f,    1.0f  },
    { "Ocean Teal",    -40.0f,  1.0f  },
    { "Emerald",       -100.0f, 1.0f  },
    { "Amber",         -170.0f, 1.0f  },
    { "Crimson",       145.0f,  1.0f  },
    { "Rose",          105.0f,  1.0f  },
    { "Violet",        55.0f,   1.0f  },
    { "Graphite",      0.0f,    0.28f }
};
static int g_theme_idx = 0;      /* 0..NPRESETS-1 = premade, NPRESETS = Custom (theme.txt) */

static const char *DEFAULT_THEME_TXT =
    "# Music Vita custom theme (pick \"Custom\" in Settings > Theme) - colors are RRGGBB hex\n"
    "# put background.png (960x544) in this folder to use your own background with Custom\n"
    "themever=2\n"
    "name=Custom\n"
    "bg=030714\n"
    "bg2=0A1F52\n"
    "header=081433\n"
    "panel=0A1636\n"
    "select=1A3C8F\n"
    "accent=4FB7FF\n"
    "text=E8F0FF\n"
    "subtext=A5B9E4\n";

static unsigned hexc(const char *s) {
    unsigned v = (unsigned)strtoul(s, NULL, 16);
    return RGBA8((v >> 16) & 255, (v >> 8) & 255, v & 255, 255);
}

static void theme_default(void) {
    T.bg = BASE[0]; T.bg2 = BASE[1]; T.header = BASE[2]; T.panel = BASE[3];
    T.select = BASE[4]; T.accent = BASE[5]; T.text = BASE[6]; T.subtext = BASE[7];
    T.bgtex = NULL;
}

/* ================= settings (saved to settings.txt) ================= */
static int g_waves = 1;
static int g_shuffle = 0;
static int g_repeat = 1;          /* 0 off, 1 all, 2 one */
static int g_lyr_adj = 0;         /* lyrics timing nudge in ms (+ = lyrics appear earlier) */
static int g_set_dirty = 0;
static SceUInt64 g_set_time = 0;

static const char *theme_name(int idx) {
    return (idx >= 0 && idx < NPRESETS) ? PRESETS[idx].name : "Custom";
}

static void settings_touch(void) {
    g_set_dirty = 1;
    g_set_time = sceKernelGetProcessTimeWide();
}

static void settings_save(void) {
    sceIoMkdir(MUSIC_DIR, 0777);
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) return;
    fprintf(f, "theme=%s\nwaves=%d\nshuffle=%d\nrepeat=%d\nlyrsync=%d\nvolume=%d\n",
            theme_name(g_theme_idx), g_waves, g_shuffle, g_repeat, g_lyr_adj, (int)g_volume);
    fclose(f);
}

static void settings_load(void) {
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[128], k[32], v[80];
    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "%31[^=]=%79[^\r\n]", k, v) != 2) continue;
        if (!strcmp(k, "theme")) {
            g_theme_idx = NPRESETS;
            for (int i = 0; i < NPRESETS; i++)
                if (!strcasecmp(v, PRESETS[i].name)) { g_theme_idx = i; break; }
        }
        else if (!strcmp(k, "waves")) g_waves = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "shuffle")) g_shuffle = atoi(v) ? 1 : 0;
        else if (!strcmp(k, "repeat")) { g_repeat = atoi(v); if (g_repeat < 0 || g_repeat > 2) g_repeat = 1; }
        else if (!strcmp(k, "lyrsync")) {
            g_lyr_adj = atoi(v);
            if (g_lyr_adj < -1500) g_lyr_adj = -1500;
            if (g_lyr_adj > 1500) g_lyr_adj = 1500;
        }
        else if (!strcmp(k, "volume")) {
            int vv = atoi(v);
            if (vv < 0) vv = 0;
            if (vv > 100) vv = 100;
            g_volume = vv;
        }
    }
    fclose(f);
}

/* ================= library ================= */
typedef struct {
    char file[256];
    char title[128], artist[128], album[128];
    char f_list[96], f_listart[96], f_tt[96], f_ta[96];
    int fitted, art_state;      /* art_state: 0 not tried, 1 loaded, 2 none, 3 loading */
    float hue;
    unsigned lru;
    vita2d_texture *art;
} Track;

static Track g_tr[MAX_TRACKS];
static int g_count = 0, g_cur = -1, g_tex_count = 0;
static unsigned g_clock = 0;

static char np_title[128], np_artist[128], np_vtitle[128], np_vartist[128];
static int g_now_dirty = 1;

static int g_vmode = 0;

static int track_cmp(const void *a, const void *b) {
    return strcasecmp(((const Track *)a)->title, ((const Track *)b)->title);
}

static void scan_library(void) {
    g_count = 0;
    sceIoMkdir(MUSIC_DIR, 0777);
    SceUID d = sceIoDopen(MUSIC_DIR);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && g_count < MAX_TRACKS) {
        if (has_ext(e.d_name, ".mp3") || has_ext(e.d_name, ".wav")) {
            Track *t = &g_tr[g_count];
            memset(t, 0, sizeof(*t));
            strncpy(t->file, e.d_name, sizeof(t->file) - 1);
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, t->file);
            Meta m;
            meta_read(path, &m, NULL, NULL, t->file);
            strncpy(t->title, m.title, 127);
            strncpy(t->artist, m.artist, 127);
            strncpy(t->album, m.album, 127);
            unsigned h = 5381;
            for (const char *c = t->artist; *c; c++) h = h * 33 + (unsigned char)*c;
            for (const char *c = t->album; *c; c++) h = h * 33 + (unsigned char)*c;
            t->hue = (float)(h % 360) / 360.0f;
            g_count++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    qsort(g_tr, g_count, sizeof(Track), track_cmp);
}

static int find_track(const char *file) {
    for (int i = 0; i < g_count; i++)
        if (!strcasecmp(g_tr[i].file, file)) return i;
    return -1;
}

/* ================= drawing helpers ================= */
static unsigned hsv(float h, float s, float v, int a) {
    h = h - floorf(h);
    float r, g, b;
    int i = (int)(h * 6.0f);
    float f = h * 6.0f - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return RGBA8((int)(r * 255), (int)(g * 255), (int)(b * 255), a);
}

static void rgb_to_hsv(unsigned c, float *h, float *s, float *v) {
    float r = (c & 255) / 255.0f, g = ((c >> 8) & 255) / 255.0f, b = ((c >> 16) & 255) / 255.0f;
    float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
    float hh = 0.0f;
    if (d > 0.0f) {
        if (mx == r) hh = fmodf((g - b) / d, 6.0f);
        else if (mx == g) hh = (b - r) / d + 2.0f;
        else hh = (r - g) / d + 4.0f;
        hh /= 6.0f;
        if (hh < 0.0f) hh += 1.0f;
    }
    *h = hh;
    *s = (mx > 0.0f) ? d / mx : 0.0f;
    *v = mx;
}

static unsigned shift_color(unsigned c, float dh, float sm) {
    float h, s, v;
    rgb_to_hsv(c, &h, &s, &v);
    h += dh / 360.0f;
    s *= sm;
    if (s > 1.0f) s = 1.0f;
    return hsv(h, s, v, 255);
}

static unsigned mixc(unsigned a, unsigned b, float t) {
    int ar = a & 255, ag = (a >> 8) & 255, ab = (a >> 16) & 255;
    int br = b & 255, bg = (b >> 8) & 255, bb = (b >> 16) & 255;
    return RGBA8((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t), 255);
}

static unsigned with_alpha(unsigned c, int a) {
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    return (c & 0x00FFFFFFu) | ((unsigned)a << 24);
}

/* shorten text with "..." so it fits maxw pixels */
static void fit(float scale, const char *src, char *dst, size_t cap, int maxw) {
    strncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
    if (text_w(scale, dst) <= maxw) return;
    size_t len = strlen(dst);
    while (len > 0) {
        len--;
        while (len > 0 && (dst[len] & 0xC0) == 0x80) len--;
        dst[len] = 0;
        char tmp[200];
        snprintf(tmp, sizeof(tmp), "%s...", dst);
        if (text_w(scale, tmp) <= maxw) {
            strncpy(dst, tmp, cap - 1);
            dst[cap - 1] = 0;
            return;
        }
    }
}

static void fit_track(Track *t) {
    if (t->fitted) return;
    t->fitted = 1;
    fit(0.95f, t->title, t->f_list, sizeof(t->f_list), 560);
    fit(0.85f, t->artist, t->f_listart, sizeof(t->f_listart), 300);
    fit(0.85f, t->title, t->f_tt, sizeof(t->f_tt), 160);
    fit(0.8f, t->artist, t->f_ta, sizeof(t->f_ta), 160);
}

/* keeps the selected row inside a scrolling window of `vis` rows */
static void list_window(int sel, int *top, int vis) {
    if (sel < *top) *top = sel;
    if (sel >= *top + vis) *top = sel - vis + 1;
    if (*top < 0) *top = 0;
}

/* ================= background: gradient + waves, both pre-rendered textures ================= */
static float s_t = 0.0f;                       /* animation clock (advances once per frame) */
static vita2d_texture *g_bg_grad = NULL;
static vita2d_texture *g_wave_tex = NULL;

static void build_bg(void) {
    if (g_bg_grad) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(g_bg_grad);
        g_bg_grad = NULL;
    }
    g_bg_grad = vita2d_create_empty_texture(8, 64);
    if (!g_bg_grad) return;
    int stride = vita2d_texture_get_stride(g_bg_grad);
    uint8_t *base = (uint8_t *)vita2d_texture_get_datap(g_bg_grad);
    for (int y = 0; y < 64; y++) {
        unsigned c = mixc(T.bg, T.bg2, y / 63.0f);
        uint32_t *row = (uint32_t *)(base + y * stride);
        for (int x = 0; x < 8; x++) row[x] = c;
    }
    vita2d_texture_set_filters(g_bg_grad, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}

/* one seamless, horizontally tileable strip with a soft glowing wave line (drawn once at startup) */
static void build_waves(void) {
    g_wave_tex = vita2d_create_empty_texture(WAVE_W, WAVE_H);
    if (!g_wave_tex) return;
    int stride = vita2d_texture_get_stride(g_wave_tex);
    uint8_t *base = (uint8_t *)vita2d_texture_get_datap(g_wave_tex);
    static float yc[WAVE_W];
    for (int x = 0; x < WAVE_W; x++) {
        float t = 6.2831853f * x / WAVE_W;
        yc[x] = WAVE_H * 0.5f + 20.0f * sinf(2.0f * t + 0.6f) + 9.0f * sinf(5.0f * t + 2.1f);
    }
    for (int y = 0; y < WAVE_H; y++) {
        uint32_t *row = (uint32_t *)(base + (size_t)y * stride);
        for (int x = 0; x < WAVE_W; x++) {
            float d = fabsf((float)y - yc[x]);
            float core = 1.0f - (d - 1.0f) / 1.5f;
            if (core > 1.0f) core = 1.0f;
            if (core < 0.0f) core = 0.0f;
            float glow = 0.30f * expf(-(d * d) / 70.0f);
            float a = core > glow ? core : glow;
            row[x] = RGBA8(255, 255, 255, (int)(a * 255.0f));
        }
    }
    vita2d_texture_set_filters(g_wave_tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
}

/* three layers, two quad draws each = 6 draws per frame, scrolled by changing the x position */
static void draw_waves(void) {
    if (!g_wave_tex || !g_waves) return;
    static const float yy[3]  = { 300.0f, 350.0f, 395.0f };
    static const float sx[3]  = { 1.0f, 1.4f, 0.8f };
    static const float sy[3]  = { 1.0f, 1.2f, 0.9f };
    static const float spd[3] = { 0.5f, -0.8f, 1.1f };      /* pixels per frame */
    static const int   al[3]  = { 70, 50, 40 };
    for (int k = 0; k < 3; k++) {
        float period = WAVE_W * sx[k];
        float off = fmodf(s_t * 60.0f * spd[k], period);
        if (off < 0.0f) off += period;
        float bob = sinf(s_t * (0.4f + 0.13f * k) + k) * 10.0f;
        unsigned col = with_alpha(T.accent, al[k]);
        vita2d_draw_texture_tint_scale(g_wave_tex, -off, yy[k] + bob, sx[k], sy[k], col);
        vita2d_draw_texture_tint_scale(g_wave_tex, -off + period, yy[k] + bob, sx[k], sy[k], col);
    }
}

static void draw_bg(int waves) {
    if (T.bgtex) {
        float sx = 960.0f / vita2d_texture_get_width(T.bgtex);
        float sy = 544.0f / vita2d_texture_get_height(T.bgtex);
        vita2d_draw_texture_scale(T.bgtex, 0, 0, sx, sy);
        return;
    }
    if (g_bg_grad) vita2d_draw_texture_scale(g_bg_grad, 0, 0, 120.0f, 8.5f);
    if (waves) draw_waves();
}

/* ================= theme apply / custom theme file ================= */
static void write_default_theme(void) {
    FILE *f = fopen(THEME_DIR "/theme.txt", "w");
    if (f) { fputs(DEFAULT_THEME_TXT, f); fclose(f); }
}

/* reads theme.txt into T_custom (T must still hold the default blue theme when this runs) */
static void theme_custom_load(void) {
    sceIoMkdir(MUSIC_DIR, 0777);
    sceIoMkdir(THEME_DIR, 0777);
    Theme def = T;
    def.bgtex = NULL;
    T_custom = def;
    FILE *f = fopen(THEME_DIR "/theme.txt", "r");
    if (!f) { write_default_theme(); return; }
    Theme tmp = def;
    int ver = 1;
    char line[128], k[32], v[64];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (sscanf(line, "%31[^=]=%63s", k, v) != 2) continue;
        if (!strcmp(k, "themever")) ver = atoi(v);
        else if (!strcmp(k, "bg")) tmp.bg = hexc(v);
        else if (!strcmp(k, "bg2")) tmp.bg2 = hexc(v);
        else if (!strcmp(k, "header")) tmp.header = hexc(v);
        else if (!strcmp(k, "panel")) tmp.panel = hexc(v);
        else if (!strcmp(k, "select")) tmp.select = hexc(v);
        else if (!strcmp(k, "accent")) tmp.accent = hexc(v);
        else if (!strcmp(k, "text")) tmp.text = hexc(v);
        else if (!strcmp(k, "subtext")) tmp.subtext = hexc(v);
    }
    fclose(f);
    if (ver >= 2) {
        T_custom = tmp;
    } else {
        sceIoRemove(THEME_DIR "/theme_v1_backup.txt");
        sceIoRename(THEME_DIR "/theme.txt", THEME_DIR "/theme_v1_backup.txt");
        write_default_theme();
    }
}

static void preset_colors(int idx, unsigned *out8) {
    const Preset *p = &PRESETS[idx];
    for (int i = 0; i < 8; i++) out8[i] = shift_color(BASE[i], p->hue, p->sat);
}

static void theme_apply(int idx) {
    g_theme_idx = idx;
    if (T.bgtex) {
        vita2d_wait_rendering_done();
        vita2d_free_texture(T.bgtex);
        T.bgtex = NULL;
    }
    if (idx >= 0 && idx < NPRESETS) {
        unsigned c[8];
        preset_colors(idx, c);
        T.bg = c[0]; T.bg2 = c[1]; T.header = c[2]; T.panel = c[3];
        T.select = c[4]; T.accent = c[5]; T.text = c[6]; T.subtext = c[7];
    } else {
        g_theme_idx = NPRESETS;
        T = T_custom;
        T.bgtex = vita2d_load_PNG_file(THEME_DIR "/background.png");
    }
    vita2d_set_clear_color(T.bg);
    build_bg();
}

/* three swatch colors (backdrop, selection, accent) for the theme list */
static void theme_preview(int idx, unsigned *c1, unsigned *c2, unsigned *c3) {
    if (idx >= 0 && idx < NPRESETS) {
        unsigned c[8];
        preset_colors(idx, c);
        *c1 = c[1]; *c2 = c[4]; *c3 = c[5];
    } else {
        *c1 = T_custom.bg2; *c2 = T_custom.select; *c3 = T_custom.accent;
    }
}

/* ================= cover art: decoded on a background thread, shrunk to ~300 px ================= */
struct jerr { struct jpeg_error_mgr pub; jmp_buf jb; };
static void jerr_exit(j_common_ptr c) { longjmp(((struct jerr *)c->err)->jb, 1); }

/* JPEG at 1/1, 1/2, 1/4 or 1/8 size (as small as it can while staying >= target) -> RGBA pixels */
static uint32_t *decode_jpeg_small(const unsigned char *data, size_t n, int target, int *ow, int *oh) {
    struct jpeg_decompress_struct cinfo;
    struct jerr jerr;
    uint32_t *volatile out = NULL;
    unsigned char *volatile row = NULL;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jerr_exit;
    if (setjmp(jerr.jb)) {
        jpeg_destroy_decompress(&cinfo);
        if (row) free(row);
        if (out) free(out);
        return NULL;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (unsigned char *)data, (unsigned long)n);
    jpeg_read_header(&cinfo, TRUE);

    int denom = 1;
    while (denom < 8 && (int)cinfo.image_width / (denom * 2) >= target &&
           (int)cinfo.image_height / (denom * 2) >= target) denom *= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    int w = (int)cinfo.output_width, h = (int)cinfo.output_height;
    if (cinfo.output_components != 3 || w <= 0 || h <= 0 || w > 4096 || h > 4096) {
        jpeg_destroy_decompress(&cinfo);
        return NULL;
    }
    out = (uint32_t *)malloc((size_t)w * h * 4);
    row = (unsigned char *)malloc((size_t)w * 3);
    if (!out || !row) {
        jpeg_destroy_decompress(&cinfo);
        if (row) free(row);
        if (out) free(out);
        return NULL;
    }
    while (cinfo.output_scanline < cinfo.output_height) {
        unsigned y = cinfo.output_scanline;
        JSAMPROW rp = (JSAMPROW)row;
        jpeg_read_scanlines(&cinfo, &rp, 1);
        uint32_t *dst = out + (size_t)y * w;
        for (int x = 0; x < w; x++)
            dst[x] = RGBA8(row[x * 3], row[x * 3 + 1], row[x * 3 + 2], 255);
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    free(row);
    *ow = w;
    *oh = h;
    return out;
}

struct mem_rd { const unsigned char *p; size_t n, pos; };
static void png_mem_read(png_structp png, png_bytep out, png_size_t len) {
    struct mem_rd *m = (struct mem_rd *)png_get_io_ptr(png);
    if (m->pos + len > m->n) png_error(png, "eof");
    memcpy(out, m->p + m->pos, len);
    m->pos += len;
}

/* adds one RGBA source row into the box-filter accumulator */
static void acc_add_row(unsigned int *acc, const unsigned char *row, int cw, int f) {
    for (int cx = 0; cx < cw; cx++) {
        unsigned r = 0, g = 0, b = 0;
        const unsigned char *p = row + (size_t)cx * f * 4;
        for (int k = 0; k < f; k++, p += 4) { r += p[0]; g += p[1]; b += p[2]; }
        acc[cx * 3] += r;
        acc[cx * 3 + 1] += g;
        acc[cx * 3 + 2] += b;
    }
}

static void acc_flush(unsigned int *acc, uint32_t *dst, int cw, int f) {
    unsigned d = (unsigned)(f * f);
    for (int x = 0; x < cw; x++) {
        dst[x] = RGBA8(acc[x * 3] / d, acc[x * 3 + 1] / d, acc[x * 3 + 2] / d, 255);
        acc[x * 3] = acc[x * 3 + 1] = acc[x * 3 + 2] = 0;
    }
}

/* PNG of any normal size: decoded row by row and averaged down, so big images stay small and fast */
static uint32_t *decode_png_small(const unsigned char *data, size_t n, int target, int *ow, int *oh) {
    struct mem_rd mr;
    mr.p = data; mr.n = n; mr.pos = 0;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return NULL;
    png_infop info = png_create_info_struct(png);
    if (!info) { png_destroy_read_struct(&png, NULL, NULL); return NULL; }

    uint32_t *volatile out = NULL;
    unsigned char *volatile row = NULL;
    unsigned int *volatile acc = NULL;
    png_bytep *volatile rows = NULL;

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (row) free(row);
        if (acc) free(acc);
        if (rows) free(rows);
        if (out) free(out);
        return NULL;
    }
    png_set_read_fn(png, &mr, png_mem_read);
    png_read_info(png, info);

    png_uint_32 w = 0, h = 0;
    int depth = 0, ctype = 0, interlace = 0;
    png_get_IHDR(png, info, &w, &h, &depth, &ctype, &interlace, NULL, NULL);
    if (w == 0 || h == 0 || w > 8192 || h > 8192) png_error(png, "size");
    if (interlace != PNG_INTERLACE_NONE && (unsigned long)w * (unsigned long)h > 3000000UL)
        png_error(png, "interlaced image too large");

    if (depth == 16) png_set_strip_16(png);
    if (ctype == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ctype == PNG_COLOR_TYPE_GRAY && depth < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ctype == PNG_COLOR_TYPE_GRAY || ctype == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_set_interlace_handling(png);
    png_read_update_info(png, info);

    int f = (int)((w < h ? w : h) / (png_uint_32)target);
    if (f < 1) f = 1;
    int cw = (int)w / f, ch = (int)h / f;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    out = (uint32_t *)malloc((size_t)cw * ch * 4);
    acc = (unsigned int *)calloc((size_t)cw * 3, sizeof(unsigned int));
    if (!out || !acc) png_error(png, "memory");

    if (interlace == PNG_INTERLACE_NONE) {
        row = (unsigned char *)malloc((size_t)w * 4);
        if (!row) png_error(png, "memory");
        int oy = 0, cnt = 0;
        for (png_uint_32 y = 0; y < h && oy < ch; y++) {
            png_read_row(png, (png_bytep)row, NULL);
            acc_add_row(acc, row, cw, f);
            if (++cnt == f) { acc_flush(acc, out + (size_t)oy * cw, cw, f); oy++; cnt = 0; }
        }
    } else {
        size_t rb = (size_t)w * 4;
        row = (unsigned char *)malloc(rb * h);
        rows = (png_bytep *)malloc(sizeof(png_bytep) * h);
        if (!row || !rows) png_error(png, "memory");
        for (png_uint_32 y = 0; y < h; y++) rows[y] = (png_bytep)row + (size_t)y * rb;
        png_bytep *rp = rows;
        png_read_image(png, rp);
        int oy = 0, cnt = 0;
        for (png_uint_32 y = 0; y < h && oy < ch; y++) {
            acc_add_row(acc, (const unsigned char *)row + (size_t)y * rb, cw, f);
            if (++cnt == f) { acc_flush(acc, out + (size_t)oy * cw, cw, f); oy++; cnt = 0; }
        }
    }
    png_destroy_read_struct(&png, &info, NULL);
    free(row);
    free(acc);
    if (rows) free(rows);
    *ow = cw;
    *oh = ch;
    return out;
}

static uint32_t *decode_image(const unsigned char *d, size_t n, int *w, int *h, int target) {
    if (n > 4 && d[0] == 0xFF && d[1] == 0xD8) return decode_jpeg_small(d, n, target, w, h);
    if (n > 8 && !memcmp(d, "\x89PNG", 4)) return decode_png_small(d, n, target, w, h);
    return NULL;
}

/* any size image file -> texture (shrunk so its short side is about `target`) */
static vita2d_texture *load_tex_scaled(const char *path, int target) {
    size_t n = 0;
    unsigned char *b = read_file(path, 24 * 1024 * 1024, &n);
    if (!b) return NULL;
    int w = 0, h = 0;
    uint32_t *pix = decode_image(b, n, &w, &h, target);
    free(b);
    if (!pix) return NULL;
    vita2d_texture *tex = vita2d_create_empty_texture((unsigned)w, (unsigned)h);
    if (tex) {
        uint8_t *base = (uint8_t *)vita2d_texture_get_datap(tex);
        int stride = vita2d_texture_get_stride(tex);
        for (int y = 0; y < h; y++)
            memcpy(base + (size_t)y * stride, pix + (size_t)y * w, (size_t)w * 4);
        vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
    }
    free(pix);
    return tex;
}

/* Song.jpg / Song.png next to the song, or cover.jpg / folder.jpg / front.jpg in the music folder */
static unsigned char *find_sidecar_art(const char *file, size_t *n) {
    char base[256];
    strncpy(base, file, sizeof(base) - 1);
    base[sizeof(base) - 1] = 0;
    char *dot = strrchr(base, '.');
    if (dot) *dot = 0;
    static const char *exts[3] = { ".jpg", ".jpeg", ".png" };
    static const char *names[3] = { "cover", "folder", "front" };
    char path[600];
    unsigned char *b;
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof(path), "%s/%s%s", MUSIC_DIR, base, exts[i]);
        b = read_file(path, MAX_ART_BYTES, n);
        if (b) return b;
    }
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 3; i++) {
            snprintf(path, sizeof(path), "%s/%s%s", MUSIC_DIR, names[k], exts[i]);
            b = read_file(path, MAX_ART_BYTES, n);
            if (b) return b;
        }
    }
    return NULL;
}

static uint32_t *load_cover(const char *file, int *ow, int *oh) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", MUSIC_DIR, file);
    Meta m;
    unsigned char *img = NULL;
    size_t n = 0;
    uint32_t *pix = NULL;
    meta_read(path, &m, &img, &n, file);
    if (img) {
        pix = decode_image(img, n, ow, oh, ART_TARGET);
        free(img);
    }
    if (!pix) {
        img = find_sidecar_art(file, &n);
        if (img) {
            pix = decode_image(img, n, ow, oh, ART_TARGET);
            free(img);
        }
    }
    return pix;
}

/* hand-off between the cover thread and the main thread (one request at a time) */
static volatile int g_art_req = -1;            /* track index the worker is decoding */
static volatile int g_art_res = -1;            /* track index whose result is waiting */
static uint32_t *volatile g_art_buf = NULL;
static volatile int g_art_w = 0, g_art_h = 0;
static char g_art_file[256];

static int art_thread(SceSize args, void *argp) {
    while (g_run) {
        int idx = g_art_req;
        if (idx < 0) { sceKernelDelayThread(8000); continue; }
        int w = 0, h = 0;
        uint32_t *pix = load_cover(g_art_file, &w, &h);
        g_art_buf = pix;
        g_art_w = pix ? w : 0;
        g_art_h = pix ? h : 0;
        __sync_synchronize();
        g_art_res = idx;
        g_art_req = -1;
    }
    return 0;
}

static void art_start(void) {
    SceUID t = sceKernelCreateThread("mv_art", art_thread, 0xB0, 0x20000, 0, 0, NULL);
    sceKernelStartThread(t, 0, NULL);
}

/* main thread: turns finished covers into textures and asks for the next one */
static void art_service(int first, int last) {
    if (g_count == 0) return;
    if (first < 0) first = 0;
    if (last >= g_count) last = g_count - 1;
    for (int i = first; i <= last; i++) g_tr[i].lru = ++g_clock;
    if (g_cur >= 0) g_tr[g_cur].lru = ++g_clock;

    if (g_art_res >= 0) {
        int idx = g_art_res;
        Track *t = &g_tr[idx];
        uint32_t *pix = g_art_buf;
        int aw = g_art_w, ah = g_art_h;
        t->art_state = 2;
        if (pix && aw > 0 && ah > 0) {
            if (g_tex_count >= MAX_TEX) {
                int ev = -1;
                unsigned best = 0xFFFFFFFFu;
                for (int i = 0; i < g_count; i++)
                    if (g_tr[i].art && g_tr[i].lru < best) { best = g_tr[i].lru; ev = i; }
                if (ev >= 0) {
                    vita2d_wait_rendering_done();
                    vita2d_free_texture(g_tr[ev].art);
                    g_tr[ev].art = NULL;
                    g_tr[ev].art_state = 0;
                    g_tex_count--;
                }
            }
            vita2d_texture *tex = vita2d_create_empty_texture((unsigned)aw, (unsigned)ah);
            if (tex) {
                uint8_t *base = (uint8_t *)vita2d_texture_get_datap(tex);
                int stride = vita2d_texture_get_stride(tex);
                for (int y = 0; y < ah; y++)
                    memcpy(base + (size_t)y * stride, pix + (size_t)y * aw, (size_t)aw * 4);
                vita2d_texture_set_filters(tex, SCE_GXM_TEXTURE_FILTER_LINEAR, SCE_GXM_TEXTURE_FILTER_LINEAR);
                t->art = tex;
                t->art_state = 1;
                g_tex_count++;
            }
        }
        if (pix) free(pix);
        g_art_buf = NULL;
        __sync_synchronize();
        g_art_res = -1;
    }
    if (g_art_req >= 0 || g_art_res >= 0) return;

    int target = -1;
    if (g_cur >= 0 && g_tr[g_cur].art_state == 0) target = g_cur;
    else for (int i = first; i <= last; i++) if (g_tr[i].art_state == 0) { target = i; break; }
    if (target < 0) return;

    strncpy(g_art_file, g_tr[target].file, sizeof(g_art_file) - 1);
    g_art_file[sizeof(g_art_file) - 1] = 0;
    g_tr[target].art_state = 3;
    __sync_synchronize();
    g_art_req = target;
}

/* draws the cover as a square (center-cropped if it isn't square) */
static void draw_art(Track *t, int x, int y, int size) {
    if (t->art) {
        int tw = vita2d_texture_get_width(t->art), th = vita2d_texture_get_height(t->art);
        int sq = tw < th ? tw : th;
        float sc = (float)size / (float)sq;
        vita2d_draw_texture_part_scale(t->art, x, y, (float)((tw - sq) / 2), (float)((th - sq) / 2),
                                       (float)sq, (float)sq, sc, sc);
    } else {
        vita2d_draw_rectangle(x, y, size, size, hsv(t->hue, 0.55f, 0.45f, 255));
        char s[2] = {0, 0};
        unsigned char ch = (unsigned char)t->title[0];
        s[0] = (ch < 128 && isalnum(ch)) ? (char)toupper(ch) : '?';
        float sc = size / 60.0f;
        int tw = text_w(sc, s);
        int th = text_h(sc, s);
        draw_text(x + (size - tw) / 2, y + size / 2 + th / 2, RGBA8(255, 255, 255, 200), sc, s);
    }
}

/* ================= lyrics display ================= */
static float lyr_y = 0.0f, lyr_tgt = 0.0f, lyr_total = 0.0f;

/* byte index where a long line should wrap onto a second row (0 = no wrap) */
static int wrap_split(const char *t, float sc, int maxw) {
    int len = (int)strlen(t);
    if (len == 0 || text_w(sc, t) <= maxw) return 0;
    int best = -1;
    char buf[130];
    for (int i = 1; i < len && i < 128; i++) {
        if (t[i] != ' ') continue;
        memcpy(buf, t, (size_t)i);
        buf[i] = 0;
        if (text_w(sc, buf) <= maxw) best = i; else break;
    }
    if (best < 0) {
        best = len / 2;
        while (best < len && ((unsigned char)t[best] & 0xC0) == 0x80) best++;
        if (best >= len) return 0;
    }
    return best;
}

static void lyrics_layout(void) {
    float y = 0.0f;
    for (int i = 0; i < g_lyr.n; i++) {
        LyrLine *L = &g_lyr.l[i];
        L->split = wrap_split(L->text, 1.75f, 800);
        L->y = y;
        y += (L->split > 0 ? 2 : 1) * 36 + 30;
    }
    lyr_total = y;
    lyr_y = 0.0f;
    lyr_tgt = 0.0f;
}

static void draw_lyr_entry(LyrLine *L, float top, float sc, unsigned col) {
    int x = 64, y = (int)top + 30;
    if (L->split > 0) {
        char r1[130];
        memcpy(r1, L->text, (size_t)L->split);
        r1[L->split] = 0;
        draw_text(x, y, col, sc, r1);
        const char *r2 = L->text + L->split + (L->text[L->split] == ' ' ? 1 : 0);
        draw_text(x, y + 36, col, sc, r2);
    } else if (L->text[0]) {
        draw_text(x, y, col, sc, L->text);
    }
}

static void draw_lyrics(void) {
    if (g_cur >= 0) {
        draw_art(&g_tr[g_cur], 24, 14, 64);
        draw_text(100, 42, T.text, 1.1f, np_title);
        draw_text(100, 66, T.subtext, 0.85f, np_artist);
    }
    if (g_lyr.n == 0) {
        const char *m = (g_cur < 0) ? "Play a song to see its lyrics" : "No lyrics found for this song";
        int w = text_w(1.1f, m);
        draw_text((960 - w) / 2, 260, T.subtext, 1.1f, m);
        return;
    }

    int cur = -1;
    if (g_lyr.timed) {
        int t = (int)g_pos_ms - 80 + g_lyr_adj;
        for (int i = 0; i < g_lyr.n; i++) {
            if (g_lyr.l[i].ms <= t) cur = i; else break;
        }
    }
    float target = g_lyr.timed ? ((cur >= 0) ? g_lyr.l[cur].y : 0.0f) : lyr_tgt;
    lyr_y += (target - lyr_y) * 0.16f;

    for (int i = 0; i < g_lyr.n; i++) {
        LyrLine *L = &g_lyr.l[i];
        float top = 190.0f + (L->y - lyr_y);
        if (top < 92.0f || top > 470.0f) continue;
        if (g_lyr.timed) {
            int d = (cur >= 0) ? abs(i - cur) : (i + 1);
            if (d == 0) {
                draw_lyr_entry(L, top, 1.75f, with_alpha(T.text, 255));
            } else {
                int a = 150 - 34 * d;
                if (a < 40) a = 40;
                draw_lyr_entry(L, top, 1.3f, with_alpha(T.text, a));
            }
        } else {
            draw_lyr_entry(L, top, 1.3f, with_alpha(T.text, 225));
        }
    }
    if (!g_lyr.timed)
        draw_text_right(936, 30, T.subtext, 0.8f, "Plain lyrics: stick up/down to scroll");
}

/* ================= playback control + queue ================= */
static int g_queue[MAX_TRACKS];
static int g_qn = 0, g_qpos = -1;
static int g_stopped = 0;

static void play_index(int idx) {
    if (idx < 0 || idx >= g_count) return;
    g_cur = idx;
    g_stopped = 0;
    snprintf(g_load_path, sizeof(g_load_path), "%s/%s", MUSIC_DIR, g_tr[idx].file);
    __sync_synchronize();
    g_load_req = 1;
    g_now_dirty = 1;
    lyrics_load(g_load_path);
    lyrics_layout();
}

static void queue_all(void) {
    g_qn = g_count;
    for (int i = 0; i < g_count; i++) g_queue[i] = i;
}

static void queue_play_pos(int pos) {
    if (pos < 0 || pos >= g_qn) return;
    g_qpos = pos;
    play_index(g_queue[pos]);
}

/* user = true when a button caused it, false when the song simply ended */
static void track_next(int user) {
    if (g_qn == 0) return;
    if (!user && g_repeat == 2) { queue_play_pos(g_qpos < 0 ? 0 : g_qpos); return; }
    int np;
    if (g_shuffle && g_qn > 1) {
        do { np = rand() % g_qn; } while (np == g_qpos);
    } else {
        np = g_qpos + 1;
        if (np >= g_qn) {
            if (g_repeat == 0 && !user) { g_stopped = 1; return; }
            np = 0;
        }
    }
    queue_play_pos(np);
}

static void track_prev(void) {
    if (g_qn == 0) return;
    if (g_loaded && g_pos_ms > 3000) { g_seek_req = 0; return; }   /* restart the song first */
    int np = g_qpos - 1;
    if (np < 0) np = g_qn - 1;
    queue_play_pos(np);
}

static void seek_by(int delta_ms) {
    if (!g_loaded) return;
    int base = (g_seek_req >= 0) ? g_seek_req : (int)g_pos_ms;
    int t = base + delta_ms;
    if (t < 0) t = 0;
    if (g_dur_ms > 1000 && t > (int)g_dur_ms - 1000) t = (int)g_dur_ms - 1000;
    g_seek_req = t;
}

static void refresh_now_playing(void) {
    if (!g_now_dirty) return;
    g_now_dirty = 0;
    if (g_cur < 0) return;
    Track *t = &g_tr[g_cur];
    fit(1.0f, t->title, np_title, sizeof(np_title), 560);
    fit(0.85f, t->artist, np_artist, sizeof(np_artist), 560);
    fit(1.5f, t->title, np_vtitle, sizeof(np_vtitle), 480);
    fit(1.1f, t->artist, np_vartist, sizeof(np_vartist), 480);
}

/* ================= playlists (.m3u files in ux0:data/MusicVita/playlists/) ================= */
typedef struct { char name[64]; int n; int idx[MAX_PL_SONGS]; } Playlist;
static Playlist g_pl[MAX_PLAYLISTS];
static int g_pl_count = 0;

static int pl_cmp(const void *a, const void *b) {
    return strcasecmp(((const Playlist *)a)->name, ((const Playlist *)b)->name);
}

static int pl_has(int pi, int track) {
    Playlist *p = &g_pl[pi];
    for (int i = 0; i < p->n; i++) if (p->idx[i] == track) return 1;
    return 0;
}

static void pl_save(int pi) {
    Playlist *p = &g_pl[pi];
    sceIoMkdir(PLAYLIST_DIR, 0777);
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.m3u", PLAYLIST_DIR, p->name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs("#EXTM3U\n", f);
    for (int i = 0; i < p->n; i++) fprintf(f, "%s\n", g_tr[p->idx[i]].file);
    fclose(f);
}

static void pl_scan(void) {
    g_pl_count = 0;
    sceIoMkdir(PLAYLIST_DIR, 0777);
    SceUID d = sceIoDopen(PLAYLIST_DIR);
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && g_pl_count < MAX_PLAYLISTS) {
        if (has_ext(e.d_name, ".m3u")) {
            Playlist *p = &g_pl[g_pl_count];
            memset(p, 0, sizeof(*p));
            strncpy(p->name, e.d_name, sizeof(p->name) - 1);
            char *dot = strrchr(p->name, '.');
            if (dot) *dot = 0;
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", PLAYLIST_DIR, e.d_name);
            size_t n = 0;
            unsigned char *raw = read_file(path, 256 * 1024, &n);
            if (raw) {
                const char *s = (const char *)raw;
                if (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) s += 3;
                while (*s) {
                    const char *le = s;
                    while (*le && *le != '\n' && *le != '\r') le++;
                    char name[300];
                    size_t len = (size_t)(le - s);
                    if (len > sizeof(name) - 1) len = sizeof(name) - 1;
                    memcpy(name, s, len);
                    name[len] = 0;
                    while (len > 0 && (name[len - 1] == ' ' || name[len - 1] == '\t')) name[--len] = 0;
                    if (len > 0 && name[0] != '#') {
                        const char *base = name;
                        for (const char *q = name; *q; q++) if (*q == '/' || *q == '\\') base = q + 1;
                        int ti = find_track(base);
                        if (ti >= 0 && p->n < MAX_PL_SONGS && !pl_has(g_pl_count, ti)) p->idx[p->n++] = ti;
                    }
                    while (*le == '\n' || *le == '\r') le++;
                    s = le;
                }
                free(raw);
            }
            g_pl_count++;
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
    qsort(g_pl, (size_t)g_pl_count, sizeof(Playlist), pl_cmp);
}

/* returns the new playlist's index, or -1 */
static int pl_new(void) {
    if (g_pl_count >= MAX_PLAYLISTS) return -1;
    char name[64];
    for (int n = 1; n < 1000; n++) {
        snprintf(name, sizeof(name), "Playlist %d", n);
        int used = 0;
        for (int i = 0; i < g_pl_count; i++) if (!strcasecmp(g_pl[i].name, name)) { used = 1; break; }
        if (!used) break;
    }
    Playlist *p = &g_pl[g_pl_count];
    memset(p, 0, sizeof(*p));
    strncpy(p->name, name, sizeof(p->name) - 1);
    g_pl_count++;
    pl_save(g_pl_count - 1);
    return g_pl_count - 1;
}

static void pl_delete(int pi) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s.m3u", PLAYLIST_DIR, g_pl[pi].name);
    sceIoRemove(path);
    for (int i = pi; i < g_pl_count - 1; i++) g_pl[i] = g_pl[i + 1];
    g_pl_count--;
}

static void pl_toggle(int pi, int track) {
    Playlist *p = &g_pl[pi];
    for (int i = 0; i < p->n; i++) {
        if (p->idx[i] == track) {
            for (int j = i; j < p->n - 1; j++) p->idx[j] = p->idx[j + 1];
            p->n--;
            pl_save(pi);
            return;
        }
    }
    if (p->n < MAX_PL_SONGS) { p->idx[p->n++] = track; pl_save(pi); }
}

/* start < 0: "play all" (random start when shuffle is on) */
static void play_playlist(int pi, int start) {
    Playlist *p = &g_pl[pi];
    if (p->n == 0) return;
    g_qn = p->n;
    for (int i = 0; i < p->n; i++) g_queue[i] = p->idx[i];
    int pos = start;
    if (pos < 0) pos = g_shuffle ? (rand() % p->n) : 0;
    queue_play_pos(pos);
}

/* ================= library tabs: state ================= */
static int tab = TAB_LIST;
static int sel = 0, list_top = 0, tile_top = 0;
static int pl_mode = 0;            /* 0 playlist list, 1 inside a playlist, 2 song picker */
static int pl_sel = 0, pl_top = 0;
static int pl_cur = -1;
static int pl_isel = 0, pl_itop = 0;
static int pl_psel = 0, pl_ptop = 0;
static int pl_confirm = 0;
#define SET_ROWS 5
static int set_mode = 0;           /* 0 main settings, 1 theme list */
static int set_sel = 0;
static int set_tsel = 0, set_ttop = 0;

/* ================= visualizer: state ================= */
typedef struct { float x, y, vx, vy, size, rot, spin, life, decay, hue; int kind; } Shape;
typedef struct { float x, y, ang, life, hue; } Laser;

static Shape g_sh[MAX_SHAPES];      /* mode 0: original shapes */
static Shape g_ps[MAX_PS];          /* mode 3: PlayStation symbols */
static Laser g_las[MAX_LASERS];     /* mode 1: laser bursts */
static float s_avg_bass = 0.02f, s_bass = 0.0f, s_level = 0.0f, s_drift = 0.0f;
static float s_bar[NBANDS], s_peak[NBANDS];
static int s_cool = 0, s_ps_big = 0;

static float base_hue(void) {
    float h = (g_cur >= 0) ? g_tr[g_cur].hue : 0.6f;
    return h + s_drift;
}

/* ---- mode 0: original shapes (unchanged) ---- */
static void spawn(float x, float y, float size, int kind, float hue) {
    for (int i = 0; i < MAX_SHAPES; i++) {
        if (g_sh[i].life <= 0.0f) {
            Shape *s = &g_sh[i];
            s->x = x; s->y = y; s->size = size; s->kind = kind; s->hue = hue;
            s->vx = ((rand() % 200) - 100) / 60.0f;
            s->vy = ((rand() % 200) - 100) / 60.0f;
            s->rot = (rand() % 628) / 100.0f;
            s->spin = ((rand() % 100) - 50) / 1000.0f;
            s->life = 1.0f;
            s->decay = 0.010f + (rand() % 100) / 6000.0f;
            return;
        }
    }
}

static void poly(float cx, float cy, float r, float rot, int n, unsigned col) {
    for (int i = 0; i < n; i++) {
        float a0 = rot + i * 6.2831853f / n, a1 = rot + (i + 1) * 6.2831853f / n;
        float x0 = cx + cosf(a0) * r, y0 = cy + sinf(a0) * r;
        float x1 = cx + cosf(a1) * r, y1 = cy + sinf(a1) * r;
        vita2d_draw_line(x0, y0, x1, y1, col);
        vita2d_draw_line(x0 + 1, y0, x1 + 1, y1, col);
        vita2d_draw_line(x0, y0 + 1, x1, y1 + 1, col);
    }
}

/* ---- shared line helpers for the other modes ---- */
static void tline(float x0, float y0, float x1, float y1, int w, unsigned c) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1.0f) return;
    float nx = -dy / len, ny = dx / len;
    for (int k = -(w / 2); k <= w / 2; k++)
        vita2d_draw_line(x0 + nx * k, y0 + ny * k, x1 + nx * k, y1 + ny * k, c);
}

/* clips a segment to the screen (plus a small margin) so no line reaches far off-screen */
static int clip_seg(float *x0, float *y0, float *x1, float *y1) {
    float t0 = 0.0f, t1 = 1.0f, dx = *x1 - *x0, dy = *y1 - *y0;
    float p[4] = { -dx, dx, -dy, dy };
    float q[4] = { *x0 + 20.0f, 980.0f - *x0, *y0 + 20.0f, 564.0f - *y0 };
    for (int i = 0; i < 4; i++) {
        if (p[i] == 0.0f) {
            if (q[i] < 0.0f) return 0;
        } else {
            float r = q[i] / p[i];
            if (p[i] < 0.0f) { if (r > t1) return 0; if (r > t0) t0 = r; }
            else             { if (r < t0) return 0; if (r < t1) t1 = r; }
        }
    }
    float ax = *x0 + t0 * dx, ay = *y0 + t0 * dy;
    float bx = *x0 + t1 * dx, by = *y0 + t1 * dy;
    *x0 = ax; *y0 = ay; *x1 = bx; *y1 = by;
    return 1;
}

static void cline(float x0, float y0, float x1, float y1, int w, unsigned c) {
    if (clip_seg(&x0, &y0, &x1, &y1)) tline(x0, y0, x1, y1, w, c);
}

static void ptoly(float cx, float cy, float r, float rot, int n, int w, unsigned col) {
    for (int i = 0; i < n; i++) {
        float a0 = rot + i * 6.2831853f / n, a1 = rot + (i + 1) * 6.2831853f / n;
        tline(cx + cosf(a0) * r, cy + sinf(a0) * r, cx + cosf(a1) * r, cy + sinf(a1) * r, w, col);
    }
}

/* ---- mode 1: lasers ---- */
static void laser_burst(void) {
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < MAX_LASERS; i++) {
            if (g_las[i].life > 0.0f) continue;
            Laser *l = &g_las[i];
            int side = rand() % 4;
            if (side == 0) { l->x = -10; l->y = (float)(rand() % 544); }
            else if (side == 1) { l->x = 970; l->y = (float)(rand() % 544); }
            else if (side == 2) { l->x = (float)(rand() % 960); l->y = -10; }
            else { l->x = (float)(rand() % 960); l->y = 554; }
            float tx = 200 + rand() % 560, ty = 120 + rand() % 300;
            l->ang = atan2f(ty - l->y, tx - l->x);
            l->hue = base_hue() + (rand() % 100) / 200.0f;
            l->life = 1.0f;
            break;
        }
    }
}

/* ---- mode 3: PlayStation symbols ---- */
static void spawn_ps(float x, float y, float size, int kind) {
    for (int i = 0; i < MAX_PS; i++) {
        if (g_ps[i].life <= 0.0f) {
            Shape *s = &g_ps[i];
            s->x = x; s->y = y; s->size = size; s->kind = kind; s->hue = 0;
            s->vx = ((rand() % 200) - 100) / 90.0f;
            s->vy = ((rand() % 200) - 100) / 90.0f;
            s->rot = ((rand() % 100) - 50) / 200.0f;
            s->spin = ((rand() % 100) - 50) / 3000.0f;
            s->life = 1.0f;
            s->decay = 0.010f + (rand() % 100) / 7000.0f;
            return;
        }
    }
}

static unsigned ps_col(int kind, int a) {
    if (kind == 0) return RGBA8(64, 205, 160, a);    /* triangle */
    if (kind == 1) return RGBA8(255, 80, 80, a);     /* circle   */
    if (kind == 2) return RGBA8(90, 150, 255, a);    /* cross    */
    return RGBA8(255, 130, 210, a);                  /* square   */
}

static void draw_symbol(int kind, float x, float y, float r, float rot, int w, unsigned col) {
    if (kind == 0) ptoly(x, y, r, rot - 1.5707963f, 3, w, col);
    else if (kind == 1) ptoly(x, y, r, 0.0f, 16, w, col);
    else if (kind == 2) {
        for (int k = 0; k < 2; k++) {
            float a = rot + 0.7853982f + k * 1.5707963f;
            tline(x + cosf(a) * r, y + sinf(a) * r, x - cosf(a) * r, y - sinf(a) * r, w, col);
        }
    } else ptoly(x, y, r * 0.9f, rot + 0.7853982f, 4, w, col);
}

/* ================= visualizer: update ================= */
static void vis_update(void) {
    float b = g_vis_bass, l = g_vis_level;
    int active = !(g_paused || !g_loaded);
    if (!active) { b = 0; l = 0; }
    s_bass += (b - s_bass) * 0.5f;
    s_level += (l - s_level) * 0.3f;
    s_avg_bass += (b - s_avg_bass) * 0.02f;
    s_drift += 0.0004f;
    s_t += 0.016f;
    if (s_cool > 0) s_cool--;

    if (b > 0.04f && b > s_avg_bass * 1.35f && s_cool == 0) {
        int n = 1 + rand() % 3;
        float size = 40.0f + b * 500.0f;
        if (size > 130.0f) size = 130.0f;
        if (g_vmode == 0) {
            for (int i = 0; i < n; i++)
                spawn(80 + rand() % 800, 70 + rand() % 400, size, rand() % 4,
                      base_hue() + (rand() % 100) / 400.0f);
        } else if (g_vmode == 1) {
            laser_burst();
        } else if (g_vmode == 3) {
            for (int i = 0; i < n; i++)
                spawn_ps(80 + rand() % 800, 70 + rand() % 400, size, rand() % 4);
            s_ps_big = (s_ps_big + 1) % 4;
        }
        s_cool = 8;
    }
    if (l > 0.03f && (rand() % 100) < (int)(l * 250.0f)) {
        if (g_vmode == 0)
            spawn(rand() % 960, rand() % 544, 10.0f + rand() % 25, rand() % 4,
                  base_hue() + (rand() % 100) / 300.0f);
        else if (g_vmode == 3)
            spawn_ps(rand() % 960, rand() % 544, 14.0f + rand() % 22, rand() % 4);
    }

    for (int i = 0; i < MAX_SHAPES; i++) {
        Shape *s = &g_sh[i];
        if (s->life > 0.0f) { s->x += s->vx; s->y += s->vy; s->rot += s->spin; s->life -= s->decay; }
    }
    for (int i = 0; i < MAX_PS; i++) {
        Shape *s = &g_ps[i];
        if (s->life > 0.0f) { s->x += s->vx; s->y += s->vy; s->rot += s->spin; s->life -= s->decay; }
    }
    for (int i = 0; i < MAX_LASERS; i++)
        if (g_las[i].life > 0.0f) g_las[i].life -= 0.05f;

    for (int i = 0; i < NBANDS; i++) {
        float tgt = active ? g_bands[i] : 0.0f;
        if (tgt > s_bar[i]) s_bar[i] += (tgt - s_bar[i]) * 0.6f;
        else s_bar[i] -= 0.035f;
        if (s_bar[i] < 0.0f) s_bar[i] = 0.0f;
        if (s_bar[i] > s_peak[i]) s_peak[i] = s_bar[i];
        else { s_peak[i] -= 0.006f; if (s_peak[i] < 0.0f) s_peak[i] = 0.0f; }
    }
}

/* ================= time bar + now-playing strip ================= */
static void fmt_time(char *dst, size_t cap, unsigned ms, int neg) {
    unsigned s = ms / 1000;
    snprintf(dst, cap, "%s%u:%02u", neg ? "-" : "", s / 60, s % 60);
}

static void draw_progress(int x, int y, int w, int h) {
    vita2d_draw_rectangle(x, y, w, h, with_alpha(T.text, 50));
    if (g_cur >= 0 && g_dur_ms > 0) {
        float f = (float)g_pos_ms / (float)g_dur_ms;
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        vita2d_draw_rectangle(x, y, (int)(w * f), h, T.accent);
        vita2d_draw_fill_circle(x + w * f, y + h / 2.0f, h + 1.0f, T.text);
    }
}

static void draw_times(int xl, int xr, int y, float sc) {
    char a[16] = "--:--", b[16] = "--:--";
    if (g_cur >= 0) {
        fmt_time(a, sizeof(a), g_pos_ms, 0);
        if (g_dur_ms > 0) {
            unsigned rem = (g_dur_ms > g_pos_ms) ? (g_dur_ms - g_pos_ms) : 0;
            fmt_time(b, sizeof(b), rem, 1);
        }
    }
    draw_text(xl, y, T.subtext, sc, a);
    draw_text_right(xr, y, T.subtext, sc, b);
}

static void draw_vis_info_small(void) {
    vita2d_draw_rectangle(0, 454, 960, 90, RGBA8(0, 0, 0, 110));
    if (g_cur >= 0) {
        draw_art(&g_tr[g_cur], 24, 462, 64);
        draw_text(100, 484, T.text, 1.0f, np_title);
        draw_text(100, 506, T.subtext, 0.85f, np_artist);
        draw_text_right(936, 484, T.accent, 0.9f, g_paused ? "Paused" : "Playing");
    }
    draw_progress(100, 518, 836, 5);
    draw_times(100, 936, 538, 0.8f);
}

/* ================= visualizers: drawing ================= */
/* mode 0: original shapes visualizer */
static void draw_vis_shapes(void) {
    int a = (int)(s_bass * 500.0f);
    if (a > 70) a = 70;
    vita2d_draw_rectangle(0, 0, 960, 544, hsv(base_hue(), 0.8f, 0.8f, a));

    for (int i = 0; i < MAX_SHAPES; i++) {
        Shape *s = &g_sh[i];
        if (s->life <= 0.0f) continue;
        int al = (int)(s->life * 200.0f);
        unsigned col = hsv(s->hue, 0.65f, 1.0f, al);
        if (s->kind == 0) poly(s->x, s->y, s->size, s->rot, 3, col);
        else if (s->kind == 1) poly(s->x, s->y, s->size, s->rot, 4, col);
        else if (s->kind == 2) poly(s->x, s->y, s->size, s->rot, 6, col);
        else vita2d_draw_fill_circle(s->x, s->y, s->size * 0.5f, hsv(s->hue, 0.65f, 1.0f, al / 2));
    }

    if (g_cur >= 0) {
        Track *t = &g_tr[g_cur];
        draw_art(t, 70, 130, 280);
        draw_text(400, 240, T.text, 1.5f, np_vtitle);
        draw_text(400, 285, T.subtext, 1.1f, np_vartist);
        char tm[32];
        snprintf(tm, sizeof(tm), "%s  %02u:%02u", g_paused ? "Paused" : "Playing",
                 (unsigned)g_seconds / 60, (unsigned)g_seconds % 60);
        draw_text(400, 330, T.accent, 0.9f, tm);
    } else {
        draw_text(400, 270, T.subtext, 1.0f, "Pick a song in the library first");
    }
}

/* mode 1: lasers */
static void draw_vis_lasers(void) {
    int fl = (int)(s_bass * 400.0f);
    if (fl > 45) fl = 45;
    vita2d_draw_rectangle(0, 0, 960, 544, hsv(base_hue() + 0.5f, 0.8f, 0.6f, fl));

    int base_a = 70 + (int)(s_level * 700.0f);
    if (base_a > 210) base_a = 210;
    int gw = (s_bass > 0.15f) ? 5 : 3;

    for (int i = 0; i < 10; i++) {
        float ex = 100.0f + (i % 5) * 190.0f;
        int top = (i >= 5);
        float ey = top ? -10.0f : 554.0f;
        float ang = (top ? 1.5707963f : -1.5707963f)
                    + sinf(s_t * (0.5f + 0.11f * i) + i * 1.3f) * 0.95f;
        float x1 = ex + cosf(ang) * 1300.0f, y1 = ey + sinf(ang) * 1300.0f;
        float hue = base_hue() + i * 0.07f;
        cline(ex, ey, x1, y1, gw, hsv(hue, 0.8f, 1.0f, base_a / 2));
        cline(ex, ey, x1, y1, 1, RGBA8(255, 255, 255, base_a));
        vita2d_draw_fill_circle(ex, ey < 0 ? 4 : 540, 8.0f + s_bass * 40.0f, hsv(hue, 0.6f, 1.0f, 160));
    }

    for (int i = 0; i < MAX_LASERS; i++) {
        Laser *l = &g_las[i];
        if (l->life <= 0.0f) continue;
        int al = (int)(l->life * 255.0f);
        float x1 = l->x + cosf(l->ang) * 1500.0f, y1 = l->y + sinf(l->ang) * 1500.0f;
        cline(l->x, l->y, x1, y1, 3, hsv(l->hue, 0.7f, 1.0f, al / 2));
        cline(l->x, l->y, x1, y1, 1, RGBA8(255, 255, 255, al));
    }
    draw_vis_info_small();
}

/* mode 2: Kenwood-style spectrum + waveform */
static void draw_vis_spectrum(void) {
    int act = !(g_paused || !g_loaded);

    float px0 = 72.0f, wid = 816.0f, prevx = 0, prevy = 0;
    for (int i = 0; i < WAVE_PTS; i += 2) {
        float v = act ? g_wave[i] : 0.0f;
        if (v > 1.0f) v = 1.0f;
        if (v < -1.0f) v = -1.0f;
        float x = px0 + wid * (float)i / (WAVE_PTS - 1);
        float y = 70.0f + v * 34.0f;
        if (i > 0) tline(prevx, prevy, x, y, 2, hsv(base_hue() + 0.1f, 0.6f, 1.0f, 220));
        prevx = x; prevy = y;
    }

    float y0 = 430.0f;
    for (int i = 0; i < NBANDS; i++) {
        float x = 72.0f + i * 34.0f;
        vita2d_draw_rectangle(x, y0 - BAR_SEGS * 18, 28, BAR_SEGS * 18, RGBA8(255, 255, 255, 18));
        int lit = (int)(s_bar[i] * BAR_SEGS);
        for (int s = 0; s < lit; s++) {
            float fr = (float)s / BAR_SEGS;
            unsigned col = fr < 0.6f ? RGBA8(60, 220, 90, 255)
                         : (fr < 0.85f ? RGBA8(240, 220, 50, 255) : RGBA8(255, 70, 60, 255));
            vita2d_draw_rectangle(x, y0 - (s + 1) * 18 + 3, 28, 15, col);
        }
        int ps = (int)(s_peak[i] * BAR_SEGS);
        if (ps > lit && ps > 0)
            vita2d_draw_rectangle(x, y0 - ps * 18 + 3, 28, 15, RGBA8(255, 255, 255, 230));
    }
    draw_vis_info_small();
}

/* mode 3: PlayStation symbols */
static void draw_vis_symbols(void) {
    int fl = (int)(s_bass * 350.0f);
    if (fl > 40) fl = 40;
    vita2d_draw_rectangle(0, 0, 960, 544, ps_col(s_ps_big, fl));

    float big = 90.0f + s_bass * 700.0f;
    if (big > 230.0f) big = 230.0f;
    draw_symbol(s_ps_big, 480, 250, big, 0.0f, 3, ps_col(s_ps_big, 70));

    for (int i = 0; i < MAX_PS; i++) {
        Shape *s = &g_ps[i];
        if (s->life <= 0.0f) continue;
        int al = (int)(s->life * 230.0f);
        draw_symbol(s->kind, s->x, s->y, s->size, s->rot, 2, ps_col(s->kind, al));
    }
    draw_vis_info_small();
}

/* mode 4: Vib Ribbon style (src/vibribbon.h) */
#include "vibribbon.h"

static void draw_visualizer(int show_label) {
    if (g_vmode == 1) draw_vis_lasers();
    else if (g_vmode == 2) draw_vis_spectrum();
    else if (g_vmode == 3) draw_vis_symbols();
    else if (g_vmode == 4) draw_vis_ribbon();
    else {
        draw_vis_shapes();
        draw_times(24, 936, 526, 0.8f);
        draw_progress(24, 532, 912, 4);
    }

    if (show_label) {
        char lab[96];
        snprintf(lab, sizeof(lab), "Visualizer %d/%d: %s   (Square: next)",
                 g_vmode + 1, VIS_COUNT, VIS_NAMES[g_vmode]);
        draw_text(24, 24, T.subtext, 0.85f, lab);
        draw_text_right(936, 24, T.subtext, 0.8f, "Select: lyrics   O: pause   L/R: skip");
    }
}

/* ================= library tabs: drawing ================= */
static void draw_list(int s, int top) {
    for (int i = 0; i < LIST_ROWS && top + i < g_count; i++) {
        int idx = top + i;
        Track *t = &g_tr[idx];
        fit_track(t);
        int y = 70 + i * ROW_H;
        if (idx == s) vita2d_draw_rectangle(12, y, 936, ROW_H, T.select);
        unsigned c = (idx == g_cur) ? T.accent : T.text;
        draw_text(24, y + 19, c, 0.95f, t->f_list);
        draw_text(640, y + 19, T.subtext, 0.85f, t->f_listart);
    }
}

static void draw_tiles(int s, int top_row) {
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < TILE_COLS; c++) {
            int idx = (top_row + r) * TILE_COLS + c;
            if (idx >= g_count) return;
            Track *t = &g_tr[idx];
            fit_track(t);
            int x = 30 + c * TILE_W, y = 72 + r * TILE_H;
            int ax = x + (TILE_W - TILE_ART) / 2, ay = y;
            if (idx == s) vita2d_draw_rectangle(ax - 5, ay - 5, TILE_ART + 10, TILE_ART + 10, T.accent);
            draw_art(t, ax, ay, TILE_ART);
            unsigned tc = (idx == g_cur) ? T.accent : T.text;
            draw_text(ax, ay + TILE_ART + 20, tc, 0.85f, t->f_tt);
            draw_text(ax, ay + TILE_ART + 38, T.subtext, 0.8f, t->f_ta);
        }
    }
}

static void draw_song_row(int y, int selected, int idx, int box, int checked) {
    Track *t = &g_tr[idx];
    fit_track(t);
    if (selected) vita2d_draw_rectangle(12, y, 936, ROW_H, T.select);
    int tx = 24;
    if (box) {
        vita2d_draw_rectangle(24, y + 5, 16, 16, checked ? T.accent : with_alpha(T.text, 70));
        tx = 52;
    }
    unsigned c = (idx == g_cur) ? T.accent : T.text;
    draw_text(tx, y + 19, c, 0.95f, t->f_list);
    draw_text(640, y + 19, T.subtext, 0.85f, t->f_listart);
}

static void draw_playlists(void) {
    char buf[96];
    if (pl_mode == 0) {
        int total = g_pl_count + 1;
        list_window(pl_sel, &pl_top, LIST_ROWS);
        for (int i = 0; i < LIST_ROWS && pl_top + i < total; i++) {
            int idx = pl_top + i, y = 70 + i * ROW_H;
            if (idx == pl_sel) vita2d_draw_rectangle(12, y, 936, ROW_H, T.select);
            if (idx == 0) {
                draw_text(24, y + 19, T.accent, 0.95f, "+ New playlist");
            } else {
                Playlist *p = &g_pl[idx - 1];
                fit(0.95f, p->name, buf, sizeof(buf), 600);
                draw_text(24, y + 19, T.text, 0.95f, buf);
                char cnt[24];
                snprintf(cnt, sizeof(cnt), "%d song%s", p->n, p->n == 1 ? "" : "s");
                draw_text_right(936, y + 19, T.subtext, 0.85f, cnt);
            }
        }
        if (g_pl_count == 0) {
            draw_text(24, 300, T.subtext, 0.9f, "No playlists yet. Choose \"+ New playlist\" and press X.");
            draw_text(24, 326, T.subtext, 0.9f, "You can also drop .m3u files into ux0:data/MusicVita/playlists/");
        }
        return;
    }
    Playlist *p = &g_pl[pl_cur];
    fit(1.1f, p->name, buf, sizeof(buf), 880);
    draw_text(24, 84, T.accent, 1.1f, buf);
    if (pl_mode == 1) {
        int total = 3 + p->n;
        list_window(pl_isel, &pl_itop, 13);
        for (int i = 0; i < 13 && pl_itop + i < total; i++) {
            int idx = pl_itop + i, y = 96 + i * ROW_H;
            if (idx == pl_isel && idx < 3) vita2d_draw_rectangle(12, y, 936, ROW_H, T.select);
            if (idx == 0) {
                char l[64];
                snprintf(l, sizeof(l), "Play all  (%d song%s)", p->n, p->n == 1 ? "" : "s");
                draw_text(24, y + 19, T.text, 0.95f, l);
            } else if (idx == 1) {
                draw_text(24, y + 19, T.text, 0.95f, "Add / remove songs");
            } else if (idx == 2) {
                if (pl_confirm) draw_text(24, y + 19, RGBA8(255, 120, 120, 255), 0.95f, "Press X again to delete this playlist");
                else draw_text(24, y + 19, T.subtext, 0.95f, "Delete playlist");
            } else {
                draw_song_row(y, idx == pl_isel, p->idx[idx - 3], 0, 0);
            }
        }
        if (p->n == 0) draw_text(24, 220, T.subtext, 0.9f, "Empty playlist. Choose \"Add / remove songs\".");
        draw_text_right(936, 84, T.subtext, 0.8f, "Stick left: back");
    } else {
        list_window(pl_psel, &pl_ptop, 13);
        for (int i = 0; i < 13 && pl_ptop + i < g_count; i++) {
            int idx = pl_ptop + i, y = 96 + i * ROW_H;
            draw_song_row(y, idx == pl_psel, idx, 1, pl_has(pl_cur, idx));
        }
        draw_text_right(936, 84, T.subtext, 0.8f, "X: add / remove   Stick left: done");
    }
}

static void draw_settings(void) {
    if (set_mode == 1) {
        int total = NPRESETS + 1;
        list_window(set_tsel, &set_ttop, 12);
        draw_text(24, 84, T.accent, 1.1f, "Themes");
        draw_text_right(936, 84, T.subtext, 0.8f, "Stick up/down: preview   X or stick left: back");
        for (int i = 0; i < 12 && set_ttop + i < total; i++) {
            int idx = set_ttop + i, y = 96 + i * 28;
            if (idx == set_tsel) vita2d_draw_rectangle(12, y, 936, 28, T.select);
            unsigned c1, c2, c3;
            theme_preview(idx, &c1, &c2, &c3);
            vita2d_draw_rectangle(28, y + 5, 18, 18, c1);
            vita2d_draw_rectangle(50, y + 5, 18, 18, c2);
            vita2d_draw_rectangle(72, y + 5, 18, 18, c3);
            draw_text(108, y + 20, T.text, 0.95f, idx < NPRESETS ? PRESETS[idx].name : "Custom (theme.txt)");
            if (idx == g_theme_idx) draw_text_right(924, y + 20, T.accent, 0.85f, "Active");
        }
        return;
    }
    static const char *labels[SET_ROWS] = { "Theme", "Waves", "Shuffle", "Repeat", "Lyrics timing" };
    static const char *rep[3] = { "Off", "All", "One" };
    for (int i = 0; i < SET_ROWS; i++) {
        int y = 78 + i * 40;
        if (i == set_sel) vita2d_draw_rectangle(12, y, 936, 36, T.select);
        draw_text(36, y + 25, T.text, 1.05f, labels[i]);
        char v[64];
        switch (i) {
            case 0: snprintf(v, sizeof(v), "%s   >", theme_name(g_theme_idx)); break;
            case 1: snprintf(v, sizeof(v), "%s", g_waves ? "On" : "Off"); break;
            case 2: snprintf(v, sizeof(v), "%s", g_shuffle ? "On" : "Off"); break;
            case 3: snprintf(v, sizeof(v), "%s", rep[g_repeat]); break;
            default: snprintf(v, sizeof(v), "%+d ms", g_lyr_adj); break;
        }
        draw_text_right(924, y + 25, T.accent, 1.05f, v);
    }
    draw_text(36, 300, T.subtext, 0.85f, "Lyrics timing: raise it if lyrics show up late, lower it if they show up early.");
    draw_text(36, 330, T.subtext, 0.85f, "Stick up/down: move     Stick left/right or X: change     X on Theme: open the list");
    draw_text(36, 400, T.subtext, 0.85f, "Music Vita " APP_VERSION "   " APP_AUTHOR);
}

static void draw_tabs(void) {
    int x = 936;
    for (int i = TAB_COUNT - 1; i >= 0; i--) {
        int w = text_w(0.9f, TAB_NAMES[i]);
        int bx = x - w - 20;
        if (i == tab) vita2d_draw_rectangle(bx, 14, w + 20, 32, T.select);
        draw_text(bx + 10, 36, i == tab ? T.text : T.subtext, 0.9f, TAB_NAMES[i]);
        x = bx - 6;
    }
}

static void draw_bar(void) {
    vita2d_draw_rectangle(0, 454, 960, 90, with_alpha(T.panel, 240));
    if (g_cur >= 0) {
        draw_art(&g_tr[g_cur], 12, 462, 74);
        draw_text(98, 484, T.text, 1.0f, np_title);
        draw_text(98, 506, T.subtext, 0.85f, np_artist);
        if (g_error) {
            draw_text_right(948, 506, RGBA8(255, 120, 120, 255), 0.85f, "Can't play this file");
        } else {
            const char *st = g_stopped ? "Stopped" : (g_paused ? "Paused" : "Playing");
            if (!g_stopped && g_loaded && !g_paused && !g_dec_done && (g_wpos - g_rpos) < 4096) st = "Buffering...";
            draw_text_right(948, 506, T.accent, 0.9f, st);
        }
    } else {
        draw_text(98, 484, T.subtext, 1.0f, "Nothing playing");
    }
    char vol[24];
    snprintf(vol, sizeof(vol), "Vol %d%%", (int)g_volume);
    draw_text_right(948, 484, T.subtext, 0.9f, vol);
    char md[40] = "";
    if (g_shuffle && g_repeat == 2) snprintf(md, sizeof(md), "Shuffle  Repeat 1");
    else if (g_shuffle) snprintf(md, sizeof(md), "Shuffle");
    else if (g_repeat == 2) snprintf(md, sizeof(md), "Repeat 1");
    else if (g_repeat == 0) snprintf(md, sizeof(md), "No repeat");
    if (md[0]) draw_text_right(948 - text_w(0.9f, vol) - 18, 484, T.subtext, 0.85f, md);

    draw_progress(98, 516, 850, 5);
    draw_times(98, 948, 538, 0.8f);

    const char *hint = "Tri Menu   Sq Views   Sel Lyrics   Start Screen off";
    int hw = text_w(0.75f, hint);
    draw_text((960 - hw) / 2, 538, T.subtext, 0.75f, hint);
}

static void draw_splash(float alpha, vita2d_texture *bg, vita2d_texture *logo) {
    int al = (int)(alpha * 255.0f);
    if (al < 0) al = 0;
    if (al > 255) al = 255;
    vita2d_start_drawing();
    vita2d_clear_screen();
    if (bg) {
        float sx = 960.0f / vita2d_texture_get_width(bg), sy = 544.0f / vita2d_texture_get_height(bg);
        vita2d_draw_texture_tint_scale(bg, 0, 0, sx, sy, RGBA8(255, 255, 255, al));
    }
    if (logo) {
        int w = vita2d_texture_get_width(logo), h = vita2d_texture_get_height(logo);
        vita2d_draw_texture_tint(logo, (960 - w) / 2, (544 - h) / 2 - 30, RGBA8(255, 255, 255, al));
    } else {
        const char *name = "MUSIC VITA";
        int tw = text_w(4.0f, name);
        draw_text((960 - tw) / 2, 275, RGBA8(255, 255, 255, al), 4.0f, name);
    }
    {
        int tw = text_w(1.1f, APP_AUTHOR);
        draw_text((960 - tw) / 2, 450, RGBA8(200, 200, 225, al), 1.1f, APP_AUTHOR);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
}

/* fires once on press, then repeats while held (first = frames before repeat, period = frames between) */
static int rep_dir(unsigned down, int *cnt, int first, int period) {
    if (!down) { *cnt = 0; return 0; }
    int c = (*cnt)++;
    if (c == 0) return 1;
    return (c >= first && ((c - first) % period) == 0);
}

/* ================= library tabs: input ================= */
static void settings_change(int row, int dir, int from_x) {
    switch (row) {
        case 0:
            if (from_x) { set_mode = 1; set_tsel = g_theme_idx; set_ttop = 0; }
            return;
        case 1: g_waves = !g_waves; break;
        case 2: g_shuffle = !g_shuffle; break;
        case 3: g_repeat = (g_repeat + (dir > 0 ? 1 : 2)) % 3; break;
        default:
            g_lyr_adj += dir * 50;
            if (g_lyr_adj < -1500) g_lyr_adj = -1500;
            if (g_lyr_adj > 1500) g_lyr_adj = 1500;
            break;
    }
    settings_touch();
}

static void set_input(int nu, int nd, int nl, int nr, unsigned pressed) {
    int x = (pressed & SCE_CTRL_CROSS) ? 1 : 0;
    if (set_mode == 1) {
        int total = NPRESETS + 1;
        if (nl || x) { set_mode = 0; return; }
        if (nd) set_tsel = (set_tsel + 1) % total;
        if (nu) set_tsel = (set_tsel + total - 1) % total;
        if (nd || nu) { theme_apply(set_tsel); settings_touch(); }
        return;
    }
    if (nd) set_sel = (set_sel + 1) % SET_ROWS;
    if (nu) set_sel = (set_sel + SET_ROWS - 1) % SET_ROWS;
    if (nr) settings_change(set_sel, +1, 0);
    if (nl) settings_change(set_sel, -1, 0);
    if (x) settings_change(set_sel, +1, 1);
}

static void pl_input(int nu, int nd, int nl, int nr, unsigned pressed) {
    (void)nr;
    int x = (pressed & SCE_CTRL_CROSS) ? 1 : 0;
    if (pl_mode == 0) {
        int total = g_pl_count + 1;
        if (nd) pl_sel = (pl_sel + 1) % total;
        if (nu) pl_sel = (pl_sel + total - 1) % total;
        if (x) {
            if (pl_sel == 0) {
                int ni = pl_new();
                if (ni >= 0) { pl_cur = ni; pl_mode = 1; pl_isel = 1; pl_itop = 0; pl_confirm = 0; }
            } else {
                pl_cur = pl_sel - 1; pl_mode = 1; pl_isel = 0; pl_itop = 0; pl_confirm = 0;
            }
        }
    } else if (pl_mode == 1) {
        Playlist *p = &g_pl[pl_cur];
        int total = 3 + p->n;
        if (nl) { pl_mode = 0; pl_confirm = 0; return; }
        if (nd) pl_isel = (pl_isel + 1) % total;
        if (nu) pl_isel = (pl_isel + total - 1) % total;
        if (nd || nu) pl_confirm = 0;
        if (x) {
            if (pl_isel == 0) {
                play_playlist(pl_cur, -1);
            } else if (pl_isel == 1) {
                pl_mode = 2; pl_psel = 0; pl_ptop = 0;
            } else if (pl_isel == 2) {
                if (pl_confirm) { pl_delete(pl_cur); pl_mode = 0; pl_sel = 0; pl_top = 0; pl_confirm = 0; }
                else pl_confirm = 1;
            } else {
                play_playlist(pl_cur, pl_isel - 3);
            }
        }
    } else {
        if (nl) { pl_mode = 1; return; }
        if (g_count == 0) return;
        if (nd) pl_psel = (pl_psel + 1) % g_count;
        if (nu) pl_psel = (pl_psel + g_count - 1) % g_count;
        if (x) pl_toggle(pl_cur, pl_psel);
    }
}

static void library_input(int nu, int nd, int nl, int nr, unsigned pressed) {
    if (pressed & SCE_CTRL_TRIANGLE) { tab = (tab + 1) % TAB_COUNT; return; }
    if (tab == TAB_LIST || tab == TAB_TILES) {
        if (g_count == 0) return;
        if (pressed & SCE_CTRL_CROSS) { queue_all(); queue_play_pos(sel); }
        if (tab == TAB_LIST) {
            if (nd) sel = (sel + 1) % g_count;
            if (nu) sel = (sel - 1 + g_count) % g_count;
        } else {
            if (nr && sel < g_count - 1) sel++;
            if (nl && sel > 0) sel--;
            if (nd) { sel += TILE_COLS; if (sel >= g_count) sel = g_count - 1; }
            if (nu && sel >= TILE_COLS) sel -= TILE_COLS;
        }
    } else if (tab == TAB_PLAYLISTS) {
        pl_input(nu, nd, nl, nr, pressed);
    } else {
        set_input(nu, nd, nl, nr, pressed);
    }
}

/* ================= main ================= */
int main(void) {
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);   /* needed for the analog sticks */
    srand((unsigned)sceKernelGetProcessTimeWide());

    vita2d_init_advanced(4 * 1024 * 1024);          /* bigger draw pool than the default */
    g_pgf = vita2d_load_default_pgf();
    g_font = vita2d_load_font_file("app0:assets/font.ttf");
    theme_default();
    vita2d_set_clear_color(T.bg);

    vita2d_texture *splash_bg = load_tex_scaled("app0:assets/splash_bg.png", 480);
    vita2d_texture *logo = vita2d_load_PNG_file("app0:assets/logo.png");

    SceUInt64 t0 = sceKernelGetProcessTimeWide();
    draw_splash(1.0f, splash_bg, logo);

    settings_load();
    theme_custom_load();
    theme_apply(g_theme_idx);
    build_waves();
    scan_library();
    pl_scan();
    queue_all();
    audio_start();
    art_start();

    for (;;) {
        float el = (float)(sceKernelGetProcessTimeWide() - t0) / 1000000.0f;
        if (el >= 2.6f) break;
        float rem = 2.6f - el;
        draw_splash(rem < 0.4f ? rem / 0.4f : 1.0f, splash_bg, logo);
    }

    int view = 0;          /* 0 = library, 1..VIS_COUNT = visualizers (Square cycles) */
    int lyrics_on = 0;
    int screen_off = 0;
    int c_up = 0, c_dn = 0, c_lf = 0, c_rt = 0;     /* d-pad repeat counters */
    int n_up = 0, n_dn = 0, n_lf = 0, n_rt = 0;     /* stick repeat counters */
    int last_vol = g_volume;
    SceCtrlData pad;
    unsigned prev = 0;

    while (1) {
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned b = pad.buttons;
        unsigned pressed = b & ~prev;
        prev = b;

        /* keep the Vita from auto-suspending while music is playing */
        if (g_loaded && !g_paused) sceKernelPowerTick(1);

        if (g_ended) {
            g_ended = 0;
            track_next(0);
        }

        /* settings are written a moment after the last change */
        if (g_volume != last_vol) { last_vol = g_volume; settings_touch(); }
        if (g_set_dirty && sceKernelGetProcessTimeWide() - g_set_time > 1500000) {
            settings_save();
            g_set_dirty = 0;
        }

        /* screen-off mode: no drawing, music keeps playing, any button wakes */
        if (screen_off) {
            sceKernelPowerTick(1);
            if (pressed) { screen_off = 0; sceKernelPowerTick(0); }
            sceKernelDelayThread(40000);
            continue;
        }

        /* D-pad: up/down = volume, left/right = skim (rewind / fast-forward) */
        int vup = rep_dir(b & SCE_CTRL_UP,    &c_up, 16, 8);
        int vdn = rep_dir(b & SCE_CTRL_DOWN,  &c_dn, 16, 8);
        int skl = rep_dir(b & SCE_CTRL_LEFT,  &c_lf, 16, 8);
        int skr = rep_dir(b & SCE_CTRL_RIGHT, &c_rt, 16, 8);
        if (vup) { int v = g_volume + 5; g_volume = v > 100 ? 100 : v; }
        if (vdn) { int v = g_volume - 5; g_volume = v < 0 ? 0 : v; }
        if (skl) seek_by(-5000);
        if (skr) seek_by(5000);

        /* analog sticks (either one): menu navigation */
        int su = (pad.ly < 64  || pad.ry < 64);
        int sd = (pad.ly > 192 || pad.ry > 192);
        int sl = (pad.lx < 64  || pad.rx < 64);
        int sr = (pad.lx > 192 || pad.rx > 192);
        int nu = rep_dir(su, &n_up, 16, 6);
        int nd = rep_dir(sd, &n_dn, 16, 6);
        int nl = rep_dir(sl, &n_lf, 16, 6);
        int nr = rep_dir(sr, &n_rt, 16, 6);

        if (pressed & SCE_CTRL_START) { scePowerRequestDisplayOff(); screen_off = 1; }
        if (pressed & SCE_CTRL_SELECT) lyrics_on = !lyrics_on;
        if (pressed & SCE_CTRL_SQUARE) {
            view = (view + 1) % (VIS_COUNT + 1);
            if (view > 0) g_vmode = view - 1;
        }
        if (pressed & SCE_CTRL_CIRCLE) { if (g_loaded) g_paused = !g_paused; }
        if (g_count > 0) {
            if (pressed & SCE_CTRL_RTRIGGER) track_next(1);
            if (pressed & SCE_CTRL_LTRIGGER) track_prev();
        }

        if (lyrics_on) {
            if (!g_lyr.timed && g_lyr.n > 0) {
                if (nd) lyr_tgt += 72.0f;
                if (nu) lyr_tgt -= 72.0f;
                float mx = lyr_total - 240.0f;
                if (mx < 0.0f) mx = 0.0f;
                if (lyr_tgt > mx) lyr_tgt = mx;
                if (lyr_tgt < 0.0f) lyr_tgt = 0.0f;
            }
        } else if (view == 0) {
            library_input(nu, nd, nl, nr, pressed);
        }

        list_window(sel, &list_top, LIST_ROWS);
        int row = sel / TILE_COLS;
        if (row < tile_top) tile_top = row;
        if (row >= tile_top + 2) tile_top = row - 1;

        int first = 0, last = -1;
        if (view == 0 && !lyrics_on && tab == TAB_TILES) {
            first = tile_top * TILE_COLS;
            last = first + 2 * TILE_COLS - 1;
        }
        art_service(first, last);
        vis_update();
        vib_update();
        refresh_now_playing();

        vita2d_start_drawing();
        vita2d_clear_screen();
        g_budget = FRAME_BUDGET;
        draw_bg(view == 0);

        if (view == 0) {
            if (lyrics_on) {
                draw_lyrics();
                draw_vis_info_small();
            } else {
                vita2d_draw_rectangle(0, 0, 960, 56, with_alpha(T.header, 235));
                draw_text(24, 38, RGBA8(255, 255, 255, 255), 1.4f, "Music Vita");
                draw_text(24 + text_w(1.4f, "Music Vita") + 12, 38, T.subtext, 0.75f, APP_VERSION);
                draw_tabs();
                if (tab == TAB_LIST || tab == TAB_TILES) {
                    if (g_count == 0) {
                        draw_text(24, 110, RGBA8(255, 200, 120, 255), 1.0f, "No MP3 or WAV files found.");
                        draw_text(24, 140, T.text, 1.0f, "Copy music to ux0:data/MusicVita/ and restart the app.");
                    } else if (tab == TAB_LIST) {
                        draw_list(sel, list_top);
                    } else {
                        draw_tiles(sel, tile_top);
                    }
                } else if (tab == TAB_PLAYLISTS) {
                    draw_playlists();
                } else {
                    draw_settings();
                }
                draw_bar();
            }
        } else {
            draw_visualizer(!lyrics_on);
            if (lyrics_on) {
                vita2d_draw_rectangle(0, 0, 960, 448, RGBA8(0, 0, 0, 150));
                draw_lyrics();
            }
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    g_run = 0;
    sceKernelDelayThread(300000);
    sceKernelExitProcess(0);
    return 0;
}
