#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "audio.h"

#define MAX_ART_BYTES (12 * 1024 * 1024)
#define MAX_TAG_BYTES (16 * 1024 * 1024)

typedef struct { char title[128], artist[128], album[128]; } Meta;

/* a byte source: a real file, or a block of memory */
typedef struct { FILE *f; const unsigned char *m; long mlen; } Src;

static unsigned int be32(const unsigned char *p) {
    return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3];
}
static unsigned int le32(const unsigned char *p) {
    return ((unsigned)p[3] << 24) | ((unsigned)p[2] << 16) | ((unsigned)p[1] << 8) | p[0];
}
static unsigned int synch32(const unsigned char *p) {
    return ((unsigned)(p[0] & 0x7f) << 21) | ((unsigned)(p[1] & 0x7f) << 14) |
           ((unsigned)(p[2] & 0x7f) << 7) | (p[3] & 0x7f);
}

static void put_utf8(char *dst, size_t cap, size_t *len, unsigned cp) {
    char t[4]; int n;
    if (cp < 0x80) { t[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { t[0] = (char)(0xC0 | (cp >> 6)); t[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else { t[0] = (char)(0xE0 | (cp >> 12)); t[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); t[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    if (*len + n + 1 > cap) return;
    memcpy(dst + *len, t, n);
    *len += n;
    dst[*len] = 0;
}

/* ID3 text frame body (first byte = encoding) -> UTF-8 */
static void decode_text(const unsigned char *d, size_t n, char *dst, size_t cap) {
    size_t len = 0;
    dst[0] = 0;
    if (n < 1) return;
    int enc = d[0];
    d++; n--;
    if (enc == 0) {
        for (size_t i = 0; i < n && d[i]; i++) put_utf8(dst, cap, &len, d[i]);
    } else if (enc == 3) {
        for (size_t i = 0; i < n && d[i] && len + 1 < cap; i++) dst[len++] = (char)d[i];
        dst[len] = 0;
    } else {
        int be = (enc == 2);
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (d[0] == 0xFE && d[1] == 0xFF) { be = 1; i = 2; }
            else if (d[0] == 0xFF && d[1] == 0xFE) { be = 0; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            unsigned cp = be ? (((unsigned)d[i] << 8) | d[i + 1]) : (((unsigned)d[i + 1] << 8) | d[i]);
            if (!cp) break;
            if (cp >= 0xD800 && cp < 0xE000) continue;
            put_utf8(dst, cap, &len, cp);
        }
    }
}

static size_t src_read(Src *s, long pos, void *dst, size_t n) {
    if (s->m) {
        if (pos < 0 || pos >= s->mlen) return 0;
        size_t av = (size_t)(s->mlen - pos);
        if (n > av) n = av;
        memcpy(dst, s->m + pos, n);
        return n;
    }
    if (fseek(s->f, pos, SEEK_SET) != 0) return 0;
    return fread(dst, 1, n, s->f);
}

/* whole file into memory (4 zero bytes appended); NULL if missing / empty / too big */
static unsigned char *read_file(const char *path, size_t maxn, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0 || (size_t)sz > maxn) { fclose(f); return NULL; }
    unsigned char *b = (unsigned char *)malloc((size_t)sz + 4);
    if (!b) { fclose(f); return NULL; }
    size_t got = fread(b, 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) { free(b); return NULL; }
    b[sz] = b[sz + 1] = b[sz + 2] = b[sz + 3] = 0;
    *n = (size_t)sz;
    return b;
}

/* undo ID3 unsynchronisation (FF 00 -> FF), in place */
static size_t deunsync(unsigned char *b, size_t n) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        b[o++] = b[i];
        if (b[i] == 0xFF && i + 1 < n && b[i + 1] == 0x00) i++;
    }
    return o;
}

/* ================= lyrics: ID3 USLT / SYLT, .lrc and .txt files ================= */
#define MAX_LYR        500
#define MAX_LYR_BYTES  (256 * 1024)

typedef struct { int ms; int split; float y; char text[120]; } LyrLine;
typedef struct { int n, timed; LyrLine l[MAX_LYR]; } Lyrics;
static Lyrics g_lyr;
static Lyrics g_lyr_bak;

static void lyr_add(int ms, const char *t, int len) {
    if (g_lyr.n >= MAX_LYR) return;
    while (len > 0 && (t[len - 1] == ' ' || t[len - 1] == '\t' || t[len - 1] == '\r' || t[len - 1] == '\n')) len--;
    while (len > 0 && (*t == ' ' || *t == '\t')) { t++; len--; }
    if (len > 119) {
        len = 119;
        while (len > 0 && ((unsigned char)t[len] & 0xC0) == 0x80) len--;
    }
    LyrLine *L = &g_lyr.l[g_lyr.n++];
    L->ms = ms; L->split = 0; L->y = 0.0f;
    memcpy(L->text, t, (size_t)len);
    L->text[len] = 0;
}

static int lyr_cmp(const void *a, const void *b) {
    int x = ((const LyrLine *)a)->ms, y = ((const LyrLine *)b)->ms;
    return (x > y) - (x < y);
}

/* "[mm:ss.xx]" at p (p points at '[') */
static int lrc_ts(const char *p, int *ms, const char **end) {
    int mm = 0, ss = 0, frac = 0, fl = 0;
    const char *q = p + 1;
    if (!isdigit((unsigned char)*q)) return 0;
    while (isdigit((unsigned char)*q)) { mm = mm * 10 + (*q - '0'); q++; }
    if (*q != ':') return 0;
    q++;
    if (!isdigit((unsigned char)*q)) return 0;
    while (isdigit((unsigned char)*q)) { ss = ss * 10 + (*q - '0'); q++; }
    if (*q == '.' || *q == ':') {
        q++;
        while (isdigit((unsigned char)*q)) {
            if (fl < 3) { frac = frac * 10 + (*q - '0'); fl++; }
            q++;
        }
    }
    if (*q != ']') return 0;
    q++;
    while (fl < 3) { frac *= 10; fl++; }
    *ms = (mm * 60 + ss) * 1000 + frac;
    *end = q;
    return 1;
}

static int lyr_looks_lrc(const char *txt) {
    int hits = 0;
    const char *p = txt;
    while (*p && hits < 2) {
        const char *q = p;
        while (*q == ' ' || *q == '\t') q++;
        int ms; const char *e;
        if (*q == '[' && lrc_ts(q, &ms, &e)) hits++;
        while (*p && *p != '\n' && *p != '\r') p++;
        while (*p == '\n' || *p == '\r') p++;
    }
    return hits >= 2;
}

/* drops word-level tags like <00:12.34> from enhanced LRC lines */
static int strip_angle(const char *s, int len, char *out, int cap) {
    int o = 0;
    for (int i = 0; i < len && o < cap - 1; i++) {
        if (s[i] == '<' && i + 1 < len && isdigit((unsigned char)s[i + 1])) {
            int j = i + 1;
            while (j < len && (isdigit((unsigned char)s[j]) || s[j] == ':' || s[j] == '.')) j++;
            if (j < len && s[j] == '>') { i = j; continue; }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
    return o;
}

static void lyr_parse_lrc(const char *txt) {
    const char *p = txt;
    int offset = 0, have_off = 0;
    char clean[512];
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        const char *q = p;
        while (q < e && (*q == ' ' || *q == '\t')) q++;
        if (!strncasecmp(q, "[offset:", 8)) { offset = atoi(q + 8); have_off = 1; }
        int ts[8], nts = 0;
        while (q < e && *q == '[' && nts < 8) {
            int ms; const char *after;
            if (!lrc_ts(q, &ms, &after)) break;
            ts[nts++] = ms;
            q = after;
        }
        if (nts > 0) {
            int cl = strip_angle(q, (int)(e - q), clean, (int)sizeof(clean));
            for (int i = 0; i < nts; i++) lyr_add(ts[i], clean, cl);
        }
        while (*e == '\n' || *e == '\r') e++;
        p = e;
    }
    if (have_off) {
        for (int i = 0; i < g_lyr.n; i++) {
            int v = g_lyr.l[i].ms - offset;
            g_lyr.l[i].ms = v < 0 ? 0 : v;
        }
    }
    qsort(g_lyr.l, (size_t)g_lyr.n, sizeof(LyrLine), lyr_cmp);
    g_lyr.timed = (g_lyr.n > 0);
}

static void lyr_parse_plain(const char *txt) {
    const char *p = txt;
    int lead = 1;
    while (*p) {
        const char *e = p;
        while (*e && *e != '\n' && *e != '\r') e++;
        int len = (int)(e - p);
        if (!(lead && len == 0)) { lyr_add(-1, p, len); lead = 0; }
        if (*e == '\r' && e[1] == '\n') e++;
        if (*e) e++;
        p = e;
    }
    g_lyr.timed = 0;
}

/* skip an ID3 content descriptor (null-terminated, width depends on encoding) */
static size_t skip_descr(const unsigned char *b, size_t i, size_t sz, int enc) {
    if (enc == 1 || enc == 2) {
        while (i + 1 < sz && !(b[i] == 0 && b[i + 1] == 0)) i += 2;
        return i + 2;
    }
    while (i < sz && b[i]) i++;
    return i + 1;
}

/* next (text, timestamp) entry of a SYLT frame */
static int sylt_next(const unsigned char *b, size_t sz, size_t *pi, int enc,
                     size_t *cs, size_t *cl, unsigned *ts) {
    size_t i = *pi;
    if (i >= sz) return 0;
    *cs = i;
    if (enc == 1 || enc == 2) {
        while (i + 1 < sz && !(b[i] == 0 && b[i + 1] == 0)) i += 2;
        *cl = i - *cs;
        i += 2;
    } else {
        while (i < sz && b[i]) i++;
        *cl = i - *cs;
        i += 1;
    }
    if (i + 4 > sz) return 0;
    *ts = be32(b + i);
    *pi = i + 4;
    return 1;
}

static void sylt_text(const unsigned char *b, size_t cs, size_t cl, int enc, char *out, size_t cap) {
    unsigned char t[300];
    if (cl > 280) cl = 280;
    t[0] = (unsigned char)enc;
    memcpy(t + 1, b + cs, cl);
    t[cl + 1] = 0;
    t[cl + 2] = 0;
    decode_text(t, cl + 1, out, cap);
}

static void lyr_parse_sylt(const unsigned char *b, size_t sz) {
    if (sz < 8) return;
    int enc = b[0], tsfmt = b[4];
    size_t i = skip_descr(b, 6, sz, enc);
    size_t cs, cl, j;
    unsigned ts;
    char piece[200];
    int has_nl = 0;

    j = i;
    while (sylt_next(b, sz, &j, enc, &cs, &cl, &ts)) {
        sylt_text(b, cs, cl, enc, piece, sizeof(piece));
        if (strchr(piece, '\n') || strchr(piece, '\r')) { has_nl = 1; break; }
    }

    g_lyr.n = 0;
    char line[200];
    size_t ll = 0;
    int line_ms = -1;
    j = i;
    while (sylt_next(b, sz, &j, enc, &cs, &cl, &ts)) {
        sylt_text(b, cs, cl, enc, piece, sizeof(piece));
        int ms = (tsfmt == 1) ? (int)(ts * 26u) : (int)ts;
        if (!has_nl) { lyr_add(ms, piece, (int)strlen(piece)); continue; }
        for (const char *p = piece; *p; p++) {
            if (*p == '\n' || *p == '\r') {
                if (ll > 0) { lyr_add(line_ms, line, (int)ll); ll = 0; }
                line_ms = -1;
            } else {
                if (line_ms < 0) line_ms = ms;
                if (ll < sizeof(line) - 1) line[ll++] = *p;
            }
        }
    }
    if (has_nl && ll > 0) lyr_add(line_ms, line, (int)ll);
    g_lyr.timed = (g_lyr.n > 0);
}

static void lyr_from_frame(Src *s, long data, unsigned long sz, int is_sylt, int unsync) {
    if (is_sylt && g_lyr.timed) return;
    unsigned char *buf = (unsigned char *)malloc(sz + 4);
    if (!buf) return;
    if (src_read(s, data, buf, sz) != sz) { free(buf); return; }
    if (unsync) sz = (unsigned long)deunsync(buf, sz);
    buf[sz] = buf[sz + 1] = buf[sz + 2] = buf[sz + 3] = 0;

    if (is_sylt) {
        lyr_parse_sylt(buf, sz);
    } else {
        int enc = buf[0];
        size_t i = skip_descr(buf, 4, sz, enc);
        if (i < sz) {
            size_t n = sz - i;
            unsigned char *tmp = (unsigned char *)malloc(n + 3);
            char *txt = (char *)malloc(n * 2 + 16);
            if (tmp && txt) {
                tmp[0] = (unsigned char)enc;
                memcpy(tmp + 1, buf + i, n);
                tmp[n + 1] = 0;
                tmp[n + 2] = 0;
                decode_text(tmp, n + 1, txt, n * 2 + 16);
                if (lyr_looks_lrc(txt)) {
                    if (!g_lyr.timed) { g_lyr.n = 0; lyr_parse_lrc(txt); }
                } else if (g_lyr.n == 0) {
                    lyr_parse_plain(txt);
                }
            }
            free(tmp);
            free(txt);
        }
    }
    free(buf);
}

static int utf8_valid(const unsigned char *s, size_t n) {
    size_t i = 0;
    while (i < n) {
        unsigned char c = s[i];
        int len = (c < 0x80) ? 1 : ((c >> 5) == 6) ? 2 : ((c >> 4) == 14) ? 3 : ((c >> 3) == 30) ? 4 : 0;
        if (!len || i + (size_t)len > n) return 0;
        for (int k = 1; k < len; k++) if ((s[i + k] & 0xC0) != 0x80) return 0;
        i += (size_t)len;
    }
    return 1;
}

/* text file bytes (UTF-8, UTF-8 with BOM, UTF-16 with BOM, or Latin-1) -> UTF-8 string */
static char *text_to_utf8(const unsigned char *raw, size_t n) {
    size_t cap = n * 3 + 8;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t len = 0;
    out[0] = 0;
    if (n >= 2 && ((raw[0] == 0xFF && raw[1] == 0xFE) || (raw[0] == 0xFE && raw[1] == 0xFF))) {
        int be = (raw[0] == 0xFE);
        for (size_t i = 2; i + 1 < n; i += 2) {
            unsigned cp = be ? (((unsigned)raw[i] << 8) | raw[i + 1]) : (((unsigned)raw[i + 1] << 8) | raw[i]);
            if (cp >= 0xD800 && cp < 0xE000) continue;
            put_utf8(out, cap, &len, cp);
        }
        return out;
    }
    size_t start = (n >= 3 && raw[0] == 0xEF && raw[1] == 0xBB && raw[2] == 0xBF) ? 3 : 0;
    if (utf8_valid(raw + start, n - start)) {
        memcpy(out, raw + start, n - start);
        out[n - start] = 0;
    } else {
        for (size_t i = start; i < n; i++) put_utf8(out, cap, &len, raw[i]);
    }
    return out;
}

/* Song.lrc (timed) or Song.txt (plain) next to the song. A timed .lrc wins over embedded lyrics. */
static int lyr_load_sidecar(const char *songpath) {
    char p[620];
    strncpy(p, songpath, sizeof(p) - 20);
    p[sizeof(p) - 20] = 0;
    char *dot = strrchr(p, '.');
    char *slash = strrchr(p, '/');
    if (!dot || (slash && dot < slash)) dot = p + strlen(p);
    static const char *exts[2] = { ".lrc", ".txt" };
    for (int i = 0; i < 2; i++) {
        strcpy(dot, exts[i]);
        size_t n = 0;
        unsigned char *raw = read_file(p, MAX_LYR_BYTES, &n);
        if (!raw) continue;
        char *txt = text_to_utf8(raw, n);
        free(raw);
        if (!txt) continue;
        if (lyr_looks_lrc(txt)) {
            memcpy(&g_lyr_bak, &g_lyr, sizeof(Lyrics));
            g_lyr.n = 0;
            g_lyr.timed = 0;
            lyr_parse_lrc(txt);
            if (g_lyr.n > 0) { free(txt); return 1; }
            memcpy(&g_lyr, &g_lyr_bak, sizeof(Lyrics));
        } else if (g_lyr.n == 0) {
            lyr_parse_plain(txt);
            if (g_lyr.n > 0) { free(txt); return 1; }
        }
        free(txt);
    }
    return 0;
}

/* ================= cover art frames ================= */
static int is_img_magic(const unsigned char *p, size_t n) {
    if (n >= 3 && p[0] == 0xFF && p[1] == 0xD8 && p[2] == 0xFF) return 1;
    if (n >= 8 && !memcmp(p, "\x89PNG", 4)) return 1;
    return 0;
}

/* APIC (v2.3/2.4) or PIC (v2.2) frame -> malloc'd image bytes (caller frees) */
static void load_apic(Src *s, long data, unsigned long sz, unsigned char **art, size_t *art_len,
                      int is_pic, int unsync) {
    if (sz < 16 || sz > MAX_ART_BYTES) return;
    unsigned char *buf = (unsigned char *)malloc(sz + 4);
    if (!buf) return;
    if (src_read(s, data, buf, sz) != sz) { free(buf); return; }
    if (unsync) sz = (unsigned long)deunsync(buf, sz);

    int enc = buf[0];
    size_t i = 1;
    if (is_pic) {
        i += 3;                                   /* 3-letter image format */
    } else {
        while (i < sz && buf[i]) i++;             /* mime type */
        i++;
    }
    i++;                                          /* picture type */
    if (i < sz) {
        if (enc == 1 || enc == 2) {
            while (i + 1 < sz && !(buf[i] == 0 && buf[i + 1] == 0)) i += 2;
            i += 2;
        } else {
            while (i < sz && buf[i]) i++;
            i++;
        }
    }
    if (i >= sz || !is_img_magic(buf + i, sz - i)) {
        /* the description was odd: look for the image start nearby */
        size_t lim = sz > 300 ? 300 : sz;
        size_t k;
        for (k = 0; k + 8 <= lim; k++) if (is_img_magic(buf + k, sz - k)) break;
        if (k + 8 > lim) { free(buf); return; }
        i = k;
    }
    size_t n = sz - i;
    memmove(buf, buf + i, n);
    *art = buf;
    *art_len = n;
}

/* does a plausible frame header (or padding / end of tag) start at p? */
static int id3_next_ok(Src *s, long p, long end) {
    if (p + 4 > end) return 1;
    unsigned char c[4];
    if (src_read(s, p, c, 4) != 4) return 0;
    if (c[0] == 0) return 1;
    for (int i = 0; i < 4; i++)
        if (!((c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= '0' && c[i] <= '9'))) return 0;
    return 1;
}

static int id3_parse(Src *s, long base, Meta *m, unsigned char **art, size_t *art_len, int want_lyrics) {
    unsigned char h[10];
    if (src_read(s, base, h, 10) != 10) return 0;
    if (memcmp(h, "ID3", 3) != 0) return 0;
    int ver = h[3], flags = h[5];
    if (ver < 2 || ver > 4) return 0;
    unsigned long tagsize = synch32(h + 6);

    /* whole-tag unsynchronisation (ID3v2.2 / v2.3): undo it in memory first */
    if ((flags & 0x80) && ver <= 3 && !s->m && (art || want_lyrics) &&
        tagsize > 0 && tagsize <= MAX_TAG_BYTES) {
        size_t total = 10 + (size_t)tagsize;
        unsigned char *raw = (unsigned char *)malloc(total);
        if (raw) {
            if (src_read(s, base, raw, total) == total) {
                size_t nb = deunsync(raw + 10, (size_t)tagsize);
                raw[5] = (unsigned char)(raw[5] & ~0x80);
                raw[6] = (unsigned char)((nb >> 21) & 0x7f);
                raw[7] = (unsigned char)((nb >> 14) & 0x7f);
                raw[8] = (unsigned char)((nb >> 7) & 0x7f);
                raw[9] = (unsigned char)(nb & 0x7f);
                Src ms;
                ms.f = NULL; ms.m = raw; ms.mlen = (long)(10 + nb);
                int r = id3_parse(&ms, 0, m, art, art_len, want_lyrics);
                free(raw);
                return r;
            }
            free(raw);
        }
    }

    long pos = base + 10;
    long end = pos + (long)tagsize;
    if ((flags & 0x40) && ver >= 3) {
        unsigned char e[4];
        if (src_read(s, pos, e, 4) != 4) return 0;
        pos += (ver == 4) ? (long)synch32(e) : (long)(be32(e) + 4);
    }
    int hdr = (ver == 2) ? 6 : 10;
    while (pos + hdr <= end) {
        unsigned char fh[10];
        if (src_read(s, pos, fh, (size_t)hdr) != (size_t)hdr) break;
        if (fh[0] == 0) break;
        char id[5] = {0};
        unsigned long sz;
        if (ver == 2) {
            memcpy(id, fh, 3);
            sz = ((unsigned long)fh[3] << 16) | ((unsigned long)fh[4] << 8) | fh[5];
        } else {
            memcpy(id, fh, 4);
            if (ver == 4) {
                /* some taggers write plain (non-synchsafe) sizes in v2.4: pick the one that fits */
                unsigned long a = synch32(fh + 4), b = be32(fh + 4);
                if ((fh[4] | fh[5] | fh[6] | fh[7]) & 0x80) {
                    sz = b;
                } else {
                    sz = a;
                    if (a != b && !id3_next_ok(s, pos + hdr + (long)a, end) &&
                        id3_next_ok(s, pos + hdr + (long)b, end)) sz = b;
                }
            } else {
                sz = be32(fh + 4);
            }
        }
        long data = pos + hdr;
        if (sz == 0 || data + (long)sz > end) break;

        long ds = data;
        unsigned long dl = sz;
        int f_unsync = 0, skip_frame = 0;
        if (ver == 4) {
            int fl = fh[9];
            if (fl & 0x0C) skip_frame = 1;                       /* compressed / encrypted */
            if (fl & 0x40) { ds += 1; dl = dl > 1 ? dl - 1 : 0; } /* grouping byte */
            if (fl & 0x01) { ds += 4; dl = dl > 4 ? dl - 4 : 0; } /* data length indicator */
            if (fl & 0x02) f_unsync = 1;
        } else if (ver == 3) {
            int fl = fh[9];
            if (fl & 0xC0) skip_frame = 1;
            if (fl & 0x20) { ds += 1; dl = dl > 1 ? dl - 1 : 0; }
        }

        if (!skip_frame && dl > 0) {
            int is_title = !strcmp(id, "TIT2") || !strcmp(id, "TT2");
            int is_artist = !strcmp(id, "TPE1") || !strcmp(id, "TP1");
            int is_album = !strcmp(id, "TALB") || !strcmp(id, "TAL");
            int is_uslt = !strcmp(id, "USLT") || !strcmp(id, "ULT");
            int is_sylt = !strcmp(id, "SYLT") || !strcmp(id, "SLT");
            int is_pic = !strcmp(id, "PIC");
            int is_apic = !strcmp(id, "APIC");
            if ((is_title || is_artist || is_album) && dl < 1024) {
                unsigned char buf[1024];
                if (src_read(s, ds, buf, dl) == dl) {
                    size_t n = dl;
                    if (f_unsync) n = deunsync(buf, n);
                    decode_text(buf, n, is_title ? m->title : (is_artist ? m->artist : m->album), 128);
                }
            } else if (want_lyrics && (is_uslt || is_sylt) && dl > 8 && dl <= MAX_LYR_BYTES) {
                lyr_from_frame(s, ds, dl, is_sylt, f_unsync);
            } else if (art && !*art && (is_apic || is_pic)) {
                load_apic(s, ds, dl, art, art_len, is_pic, f_unsync);
            }
        }
        pos = data + (long)sz;
    }
    return 1;
}

static void copy_tag(char *dst, const char *src) { strncpy(dst, src, 127); dst[127] = 0; }

static void wav_meta(Src *s, Meta *m, unsigned char **art, size_t *art_len, int want_lyrics) {
    unsigned char h[12];
    if (src_read(s, 0, h, 12) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) return;
    long pos = 12;
    for (int guard = 0; guard < 256; guard++) {
        unsigned char ch[8];
        if (src_read(s, pos, ch, 8) != 8) break;
        unsigned long sz = le32(ch + 4);
        if (sz > 0x7fffffffUL) break;
        long data = pos + 8;
        if (!memcmp(ch, "LIST", 4) && sz >= 4) {
            unsigned char t[4];
            if (src_read(s, data, t, 4) == 4 && !memcmp(t, "INFO", 4)) {
                long p = data + 4, e = data + (long)sz;
                while (p + 8 <= e) {
                    unsigned char sh[8];
                    if (src_read(s, p, sh, 8) != 8) break;
                    unsigned long ss = le32(sh + 4);
                    if (ss < 200 && p + 8 + (long)ss <= e) {
                        char buf[256];
                        size_t got = src_read(s, p + 8, buf, ss);
                        buf[got] = 0;
                        if (!memcmp(sh, "INAM", 4)) copy_tag(m->title, buf);
                        else if (!memcmp(sh, "IART", 4)) copy_tag(m->artist, buf);
                        else if (!memcmp(sh, "IPRD", 4)) copy_tag(m->album, buf);
                    }
                    p += 8 + (long)ss + (long)(ss & 1);
                }
            }
        } else if (!memcmp(ch, "id3 ", 4) || !memcmp(ch, "ID3 ", 4)) {
            id3_parse(s, data, m, art, art_len, want_lyrics);
        }
        pos = data + (long)sz + (long)(sz & 1);
    }
}

/* Reads tags (and optionally the cover image). Falls back to the file name. */
static void meta_read(const char *path, Meta *m, unsigned char **art, size_t *art_len, const char *fallback) {
    m->title[0] = m->artist[0] = m->album[0] = 0;
    if (art) { *art = NULL; *art_len = 0; }
    FILE *f = fopen(path, "rb");
    if (f) {
        Src s;
        s.f = f; s.m = NULL; s.mlen = 0;
        if (has_ext(path, ".mp3")) id3_parse(&s, 0, m, art, art_len, 0);
        else wav_meta(&s, m, art, art_len, 0);
        fclose(f);
    }
    if (!m->title[0]) {
        copy_tag(m->title, fallback);
        char *dot = strrchr(m->title, '.');
        if (dot) *dot = 0;
    }
    if (!m->artist[0]) strcpy(m->artist, "Unknown Artist");
}

/* Loads lyrics for one song into g_lyr: embedded first, then a Song.lrc / Song.txt next to it. */
static void lyrics_load(const char *path) {
    g_lyr.n = 0;
    g_lyr.timed = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        Meta m;
        memset(&m, 0, sizeof(m));
        Src s;
        s.f = f; s.m = NULL; s.mlen = 0;
        if (has_ext(path, ".mp3")) id3_parse(&s, 0, &m, NULL, NULL, 1);
        else wav_meta(&s, &m, NULL, NULL, 1);
        fclose(f);
    }
    lyr_load_sidecar(path);
}
