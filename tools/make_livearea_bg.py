import sys
from PIL import Image

src = sys.argv[1]
dst = sys.argv[2]
im = Image.open(src).convert("RGB")
print("original size:", im.size)
im = im.resize((840, 500), Image.LANCZOS)
im = im.quantize(colors=256)
im.save(dst, optimize=True)
print("wrote", dst)
