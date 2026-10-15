# Material maps for the Master Sword and sheath from the source base color and rm maps.
# usage: make_sword_maps.py <prefix of *_basecolor.png/_rm.png/_normal.png> <out dir>
# Writes basecolor.rgba32hd.png, normal.rgba32.png and rm.rgba32.png (glTF layout: G roughness, B metalness).
import sys, numpy as np
from PIL import Image, ImageFilter
src, out = sys.argv[1], sys.argv[2]
base_img = Image.open(f'{src}_basecolor.png').convert('RGBA')
base = np.asarray(base_img.convert('RGB')).astype(np.float32) / 255
rm0 = np.asarray(Image.open(f'{src}_rm.png').convert('RGB')).astype(np.float32) / 255
mx = base.max(2); mn = base.min(2); v = mx; s = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0)
r, g, b = base[..., 0], base[..., 1], base[..., 2]
d = np.maximum(mx - mn, 1e-4)
h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) / 6
h = np.where(mx - mn < 1e-4, 0, h)

# class:            leather  steel  gold  lacquer  guard  grip
rough = np.array([0.7, 0.18, 0.3, 0.32, 0.28, 0.75], np.float32)
metal = np.array([0.0, 1.0, 1.0, 0.0, 0.8, 0.0], np.float32)
cls = np.zeros(base.shape[:2], np.uint8)                               # brown leather and anything else
cls[(h > 0.05) & (h < 0.17) & (s > 0.4) & (v > 0.35)] = 2             # gold trim
cls[(h >= 0.17) & (h < 0.45) & (s > 0.2)] = 5                         # green and yellow grip wrap
cls[(h >= 0.55) & (h < 0.68) & (s > 0.3)] = 3                         # blue sheath lacquer
cls[(h >= 0.68) & (h < 0.85) & (s > 0.2)] = 4                         # violet crossguard and pommel
cls[(s < 0.2) & (v > 0.45)] = 1                                       # polished blade steel
cls = np.asarray(Image.fromarray(cls).filter(ImageFilter.ModeFilter(5)))
R = np.clip(rough[cls] + (rm0[..., 1] - rm0[..., 1].mean()) * 0.2, 0.05, 1.0)
M = metal[cls]
blur = lambda a: np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.0)))
rm = np.zeros(base.shape[:2] + (4,), np.uint8); rm[..., 0] = 255; rm[..., 1] = blur(R); rm[..., 2] = blur(M); rm[..., 3] = 255
Image.fromarray(rm).save(f'{out}/rm.rgba32.png')
base_img.save(f'{out}/basecolor.rgba32hd.png')
Image.open(f'{src}_normal.png').convert('RGBA').save(f'{out}/normal.rgba32.png')
print({n: int((cls == i).sum()) for i, n in enumerate(['leather', 'steel', 'gold', 'lacquer', 'guard', 'grip'])})
