# Procedural eye texture for the eyeballs (front-projected: centre = pupil, radius 0.5 = equator).
# Writes eye_basecolor (sclera, blue iris with fibres, limbal ring baked in, pupil) and eye_normal.
import sys, numpy as np
from PIL import Image
out = sys.argv[1]
S = 512
y, x = np.mgrid[0:S, 0:S].astype(np.float32)
u = (x + 0.5) / S - 0.5; v = 0.5 - (y + 0.5) / S
r = np.hypot(u, v) * 2          # 0 centre .. 1 equator (projected)
a = np.arctan2(v, u)
rng = np.random.default_rng(7)
def fbm(freq, octaves, seed):
    g = np.zeros((S, S), np.float32); amp = 1; tot = 0
    for o in range(octaves):
        n = rng.standard_normal((freq * 2 ** o + 1, freq * 2 ** o + 1)).astype(np.float32)
        im = np.asarray(Image.fromarray(n).resize((S, S), Image.BICUBIC))
        g += amp * im; tot += amp; amp *= 0.5
    return g / tot
IRIS = 0.47   # iris radius as a fraction of the projected eyeball radius (big, friendly eyes)
PUPIL = 0.21
# sclera: warm white, a little darker and pinker towards the corners, faint veins
scl = np.stack([np.full_like(r, 0.86), np.full_like(r, 0.83), np.full_like(r, 0.80)], -1)
scl *= (1 - 0.18 * np.clip((r - 0.55) / 0.45, 0, 1) ** 2)[..., None]
veins = np.clip(fbm(24, 3, 1) * 3 - 2.2, 0, 1) * np.clip((r - 0.62) / 0.3, 0, 1)
scl[..., 1] -= 0.05 * veins; scl[..., 2] -= 0.04 * veins
# iris: radial fibres in two blues, darker collarette, golden flecks near the pupil
ri = r / IRIS
fib = np.sin(a * 90 + fbm(8, 3, 2) * 6) * 0.5 + 0.5
fib2 = fbm(40, 2, 3)
base1 = np.array([0.07, 0.20, 0.45]); base2 = np.array([0.24, 0.43, 0.68])
t = np.clip(0.35 + 0.45 * fib + 0.25 * fib2, 0, 1)
iris = base1 * (1 - t[..., None]) + base2 * t[..., None]
coll = np.exp(-((ri - 0.45) / 0.1) ** 2)
iris = iris * (1 - 0.35 * coll[..., None])
gold = np.exp(-((ri - 0.34) / 0.08) ** 2) * np.clip(fbm(16, 2, 4) + 0.3, 0, 1)
iris = iris * (1 - 0.4 * gold[..., None]) + np.array([0.55, 0.45, 0.2]) * 0.4 * gold[..., None]
limbal = np.clip((ri - 0.82) / 0.18, 0, 1) ** 1.5          # dark ring baked in (no parallax in the renderer)
iris = iris * (1 - 0.75 * limbal[..., None])
col = np.where((ri < 1.0)[..., None], iris, scl)
edge = np.clip((ri - 0.97) / 0.06, 0, 1)                    # soften iris/sclera edge
col = iris * (1 - edge[..., None]) * (ri < 1.03)[..., None] + col * (edge[..., None] * (ri < 1.03)[..., None] + (ri >= 1.03)[..., None])
pup = np.clip((PUPIL - r) / 0.012 + 0.5, 0, 1)
col = col * (1 - pup[..., None]) + np.array([0.015, 0.015, 0.02]) * pup[..., None]
img = (np.clip(col, 0, 1) ** (1 / 1.0) * 255).astype(np.uint8)
rgba = np.dstack([img, np.full((S, S), 255, np.uint8)])
Image.fromarray(rgba).save(f'{out}/eye_basecolor.png')
# normal map: iris slightly concave, fibres give a little relief (OpenGL convention)
h = np.where(ri < 1, 0.03 * (ri ** 2) + 0.01 * fib, 0.03) - 0.02 * pup
gy, gx = np.gradient(h * 40)
n = np.dstack([-gx, gy, np.ones_like(gx)]); n /= np.linalg.norm(n, axis=2, keepdims=True)
nm = ((n * 0.5 + 0.5) * 255).astype(np.uint8)
Image.fromarray(np.dstack([nm, np.full((S, S), 255, np.uint8)])).save(f'{out}/eye_normal.png')
print('ok')
