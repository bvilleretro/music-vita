#pragma once
/* Vib Ribbon-style visualizer. Included by main.c after the helpers it uses. */

#define RIB_MAX    16
#define RIB_Y0     300.0f
#define RIB_CHARX  240.0f

typedef struct { float x, h; int kind, on; } RibObs;   /* kind: 0 spike, 1 block, 2 dip */
static RibObs g_rib[RIB_MAX];
static float r_avg = 0.02f, r_speed = 0.0f;
static int r_cool = 0, r_hop = 0;

static void rib_spawn(int kind, float h) {
    for (int i = 0; i < RIB_MAX; i++) {
        if (!g_rib[i].on) {
            g_rib[i].on = 1; g_rib[i].x = 1010.0f; g_rib[i].kind = kind; g_rib[i].h = h;
            return;
        }
    }
}

static float rib_y(float x) {
    float y = RIB_Y0;
    for (int i = 0; i < RIB_MAX; i++) {
        if (!g_rib[i].on) continue;
        float ax = fabsf(x - g_rib[i].x);
        if (g_rib[i].kind == 0) {
            if (ax < 36.0f) y -= g_rib[i].h * (1.0f - ax / 36.0f);
        } else if (g_rib[i].kind == 1) {
            if (ax < 34.0f) {
                float e = (34.0f - ax) / 6.0f;
                if (e > 1.0f) e = 1.0f;
                y -= g_rib[i].h * 0.7f * e;
            }
        } else {
            if (ax < 40.0f) y += g_rib[i].h * 0.6f * (1.0f - ax / 40.0f);
        }
    }
    return y;
}

static float rib_wave(float x) {
    if (g_paused || !g_loaded) return 0.0f;
    int i = (int)(x * (WAVE_PTS - 1) / 960.0f);
    if (i < 0) i = 0;
    if (i > WAVE_PTS - 1) i = WAVE_PTS - 1;
    return g_wave[i] * 22.0f;
}

static void vib_update(void) {
    if (g_vmode != 4) return;
    int active = !(g_paused || !g_loaded);
    float b = active ? g_vis_bass : 0.0f;
    float l = active ? g_vis_level : 0.0f;
    r_avg += (b - r_avg) * 0.02f;
    r_speed = active ? 3.5f + l * 9.0f : 0.0f;
    if (r_speed > 8.0f) r_speed = 8.0f;

    float maxx = -1000.0f;
    for (int i = 0; i < RIB_MAX; i++) {
        if (!g_rib[i].on) continue;
        g_rib[i].x -= r_speed;
        if (g_rib[i].x < -80.0f) g_rib[i].on = 0;
        else if (g_rib[i].x > maxx) maxx = g_rib[i].x;
    }
    if (r_cool > 0) r_cool--;

    if (active && r_cool == 0 && maxx < 760.0f) {
        int beat = (b > 0.04f && b > r_avg * 1.35f);
        int hit = (!beat && l > 0.10f && (rand() % 100) < 3);
        if (beat) {
            float h = 40.0f + b * 300.0f;
            if (h > 80.0f) h = 80.0f;
            rib_spawn((rand() % 4 == 0) ? 1 : 0, h);
            r_cool = 14;
        } else if (hit) {
            rib_spawn((rand() % 2) ? 2 : 1, 36.0f + (float)(rand() % 20));
            r_cool = 14;
        }
    }

    if (r_hop > 0) {
        r_hop--;
    } else {
        for (int i = 0; i < RIB_MAX; i++) {
            if (!g_rib[i].on || g_rib[i].kind == 2) continue;
            float d = g_rib[i].x - 36.0f - RIB_CHARX;
            if (d > -6.0f && d < 30.0f) { r_hop = 24; break; }
        }
    }
}

static void draw_vis_ribbon(void) {
    int active = !(g_paused || !g_loaded);
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(0, 0, 0, 215));
    int flash = (int)(s_bass * 300.0f);
    if (flash > 40) flash = 40;
    if (flash > 0) vita2d_draw_rectangle(0, 0, 960, 544, hsv(base_hue(), 0.7f, 1.0f, flash));

    unsigned glow = hsv(base_hue(), 0.5f, 1.0f, 90);
    float px0 = 0.0f, py0 = 0.0f;
    for (int x = 0; x <= 960; x += 10) {
        float y = rib_y((float)x) + rib_wave((float)x);
        if (x > 0) {
            tline(px0, py0, (float)x, y, 3, glow);
            tline(px0, py0, (float)x, y, 1, RGBA8(255, 255, 255, 255));
        }
        px0 = (float)x; py0 = y;
    }

    /* the runner */
    float hop = 0.0f;
    if (r_hop > 0) hop = sinf((24 - r_hop) / 24.0f * 3.14159265f) * 28.0f;
    float cx = RIB_CHARX;
    float cy = rib_y(cx) + rib_wave(cx) - hop;
    unsigned w = RGBA8(255, 255, 255, 255);
    float run = active ? s_t * 14.0f : 0.0f;
    float l1 = sinf(run) * 8.0f, l2 = sinf(run + 3.14159265f) * 8.0f;
    tline(cx - 2, cy - 12, cx - 4 + l1, cy, 2, w);          /* legs */
    tline(cx + 4, cy - 12, cx + 6 + l2, cy, 2, w);
    ptoly(cx, cy - 18, 9.0f, 0.0f, 14, 2, w);               /* body */
    ptoly(cx + 11, cy - 27, 6.0f, 0.0f, 12, 2, w);          /* head */
    tline(cx + 9, cy - 32, cx + 3, cy - 46, 2, w);          /* ears */
    tline(cx + 13, cy - 32, cx + 15, cy - 47, 2, w);
    ptoly(cx - 11, cy - 20, 3.0f, 0.0f, 8, 2, w);           /* tail */

    draw_vis_info_small();
}
