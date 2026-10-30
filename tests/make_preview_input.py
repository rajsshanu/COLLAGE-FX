"""Generate the synthetic poster used by the development previews."""
from PIL import Image, ImageDraw, ImageFont, ImageFilter
from array import array
import math
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "data")
os.makedirs(OUT, exist_ok=True)

W = H = 1200
img = Image.new("RGB", (W, H))
px = img.load()
for y in range(H):
    for x in range(W):
        dx = (x - W * 0.52) / W
        dy = (y - H * 0.46) / H
        r = math.sqrt(dx * dx + dy * dy)
        t = max(0.0, min(1.0, r * 1.35))
        r0, g0, b0 = 42, 23, 52
        r1, g1, b1 = 246, 92, 47
        r2, g2, b2 = 248, 225, 183
        if t < 0.55:
            u = t / 0.55
            c = (int(r0 + (r1 - r0) * u), int(g0 + (g1 - g0) * u),
                 int(b0 + (b1 - b0) * u))
        else:
            u = (t - 0.55) / 0.45
            c = (int(r1 + (r2 - r1) * u), int(g1 + (g2 - g1) * u),
                 int(b1 + (b2 - b1) * u))
        px[x, y] = c

d = ImageDraw.Draw(img, "RGBA")
d.ellipse((210, 170, 990, 950), fill=(12, 73, 78, 210))
d.ellipse((300, 250, 900, 850), fill=(236, 202, 150, 255))
d.ellipse((390, 330, 810, 750), fill=(25, 132, 139, 255))
d.ellipse((470, 410, 730, 670), fill=(244, 102, 65, 255))
d.rectangle((80, 870, 1120, 1110), fill=(24, 24, 33, 220))
for i in range(8):
    d.rectangle((80, 870 + i * 30, 1120, 884 + i * 30),
                fill=(249, 224, 181, 180 if i % 2 == 0 else 70))

font_path = r"C:\Windows\Fonts\arialbd.ttf"
f1 = ImageFont.truetype(font_path, 130)
f2 = ImageFont.truetype(font_path, 40)
d.text((95, 910), "MIXED", font=f1, fill=(250, 236, 207, 255),
       stroke_width=2, stroke_fill=(20, 20, 28, 255))
d.text((102, 1035), "M E D I A   /   P R I N T   S T U D Y", font=f2,
       fill=(250, 236, 207, 220))

grain = Image.effect_noise((W, H), 18).convert("L")
grain_rgb = Image.merge("RGB", (grain, grain, grain))
img = Image.blend(img, grain_rgb, 0.055)
img.save(os.path.join(OUT, "synthetic.jpg"), quality=95)
raw = img.convert("RGBA").transpose(Image.FLIP_TOP_BOTTOM).tobytes()
a = array("f", (b / 255.0 for b in raw))
with open(os.path.join(OUT, "synthetic.rgba"), "wb") as f:
    f.write(a.tobytes())
print("wrote", os.path.join(OUT, "synthetic.rgba"))
