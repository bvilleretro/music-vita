import colorsys
from PIL import Image

def pick_base(path):
    """most common clearly-colored shade in the icon (falls back to a plain blue)"""
    try:
        im = Image.open(path).convert("RGBA").resize((48, 48))
    except Exception:
        return (30, 80, 200)
    px = [p for p in im.getdata() if p[3] > 200]
    if not px:
        return (30, 80, 200)
    buckets = {}
    for r, g, b, a in px:
        buckets.setdefault((r // 24, g // 24, b // 24), []).append((r, g, b))
    best, best_n = None, 0
    for lst in buckets.values():
        r = sum(c[0] for c in lst) / len(lst)
        g = sum(c[1] for c in lst) / len(lst)
        b = sum(c[2] for c in lst) / len(lst)
        h, s, v = colorsys.rgb_to_hsv(r / 255, g / 255, b / 255)
        if s < 0.25 or v < 0.2 or v > 0.97:
            continue
        if len(lst) > best_n:
            best, best_n = (r, g, b), len(lst)
    if best is None:
        n = len(px)
        best = (sum(p[0] for p in px) / n, sum(p[1] for p in px) / n, sum(p[2] for p in px) / n)
    return best

def make(w, h, base):
    r0, g0, b0 = base
    img = Image.new("RGB", (w, h))
    pix = img.load()
    for y in range(h):
        k = 0.30 + 0.85 * (y / (h - 1))            # dark at the top, full color at the bottom
        for x in range(w):
            dx = (x / (w - 1)) * 2 - 1
            f = k * (1.0 - 0.22 * dx * dx)         # slightly darker toward the left/right edges
            pix[x, y] = (min(255, int(r0 * f)), min(255, int(g0 * f)), min(255, int(b0 * f)))
    return img.quantize(colors=256)

base = pick_base("sce_sys/icon0.png")
print("base color from icon0:", tuple(int(c) for c in base))
make(840, 500, base).save("sce_sys/livearea/contents/bg.png", optimize=True)
make(960, 544, base).save("sce_sys/pic0.png", optimize=True)
print("wrote bg.png and pic0.png")
