import zlib, struct, os

def chunk(tag, data):
    c = struct.pack('>I', len(data)) + tag + data
    return c + struct.pack('>I', zlib.crc32(tag + data) & 0xffffffff)

def write_png(path, w, h, top, bottom):
    if os.path.exists(path):
        print("keeping", path)
        return
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pal = b''
    for i in range(256):
        t = i / 255.0
        pal += bytes(int(top[k] + (bottom[k] - top[k]) * t) for k in range(3))
    raw = bytearray()
    for y in range(h):
        idx = int(255 * y / max(1, h - 1))
        raw += b'\x00' + bytes([idx]) * w
    png = (b'\x89PNG\r\n\x1a\n'
           + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 3, 0, 0, 0))
           + chunk(b'PLTE', pal)
           + chunk(b'IDAT', zlib.compress(bytes(raw), 9))
           + chunk(b'IEND', b''))
    open(path, 'wb').write(png)
    print("wrote", path)

top, bot = (60, 30, 130), (10, 10, 30)
write_png('sce_sys/icon0.png', 128, 128, top, bot)
write_png('sce_sys/pic0.png', 960, 544, top, bot)
write_png('sce_sys/livearea/contents/bg.png', 840, 500, top, bot)
write_png('sce_sys/livearea/contents/startup.png', 280, 158, top, bot)
