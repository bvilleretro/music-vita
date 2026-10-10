p = 'src/main.c'
s = open(p).read()

def rep(old, new):
    global s
    if s.count(old) != 1:
        raise SystemExit('PATCH FAILED, could not find: ' + old.strip()[:60])
    s = s.replace(old, new)

rep('#define APP_VERSION "v0.3"', '#define APP_VERSION "v0.4"')
rep('#define VIS_COUNT   4', '#define VIS_COUNT   5')
rep('{ "Shapes", "Lasers", "Spectrum", "Symbols" }', '{ "Shapes", "Lasers", "Spectrum", "Symbols", "Ribbon" }')
rep('static void draw_visualizer(void) {', '#include "vibribbon.h"\n\nstatic void draw_visualizer(void) {')
rep('    else if (g_vmode == 3) draw_vis_symbols();', '    else if (g_vmode == 3) draw_vis_symbols();\n    else if (g_vmode == 4) draw_vis_ribbon();')
rep('        vis_update();\n', '        vis_update();\n        vib_update();\n')
rep('        if (pressed & SCE_CTRL_SQUARE) screen = !screen;', '        if ((pressed & SCE_CTRL_SQUARE) && !(pad.buttons & SCE_CTRL_SELECT)) screen = !screen;')
rep('            if (pressed & SCE_CTRL_TRIANGLE) g_dbg = !g_dbg;', '            if (pressed & SCE_CTRL_TRIANGLE) g_dbg = !g_dbg;\n            if (pressed & SCE_CTRL_SQUARE) g_tone = !g_tone;')
rep('"buffer %.1fs   decode %.1fx realtime   underruns %u",\n             used / (float)OUT_RATE, g_dec_x10 / 10.0f, (unsigned)g_underruns);',
    '"buffer %.1fs   decode %.1fx realtime   underruns %u%s",\n             used / (float)OUT_RATE, g_dec_x10 / 10.0f, (unsigned)g_underruns, g_tone ? "   TONE ON" : "");')

open(p, 'w').write(s)
print('main.c patched OK')
