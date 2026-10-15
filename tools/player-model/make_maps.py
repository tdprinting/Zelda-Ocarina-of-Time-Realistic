# Builds the material maps for the player model from the base color (and Tripo's rm map):
# a surface class per texel, a glTF roughness/metal map, a skin mask, and a class preview.
import sys, numpy as np
from PIL import Image, ImageFilter
import colorsys
src, outdir = sys.argv[1], sys.argv[2]
base = np.asarray(Image.open(f'{src}_basecolor.png').convert('RGB')).astype(np.float32) / 255
rm0 = np.asarray(Image.open(f'{src}_rm.png').convert('RGB')).astype(np.float32) / 255
H, W, _ = base.shape
mx = base.max(2); mn = base.min(2); v = mx; s = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0)
r, g, b = base[..., 0], base[..., 1], base[..., 2]
d = np.maximum(mx - mn, 1e-4)
h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) / 6
h = np.where(mx - mn < 1e-4, 0, h)

NONE, CLOTH, LINEN, SKIN, HAIR, LEATHER, METAL = 0, 1, 2, 3, 4, 5, 6
cls = np.full((H, W), LEATHER, np.uint8)
green = (h > 0.17) & (h < 0.45) & (s > 0.15)
hair = (h > 0.09) & (h <= 0.17) & (s > 0.35) & (v > 0.45)
skin = (h > 0.0) & (h <= 0.09) & (s > 0.15) & (s < 0.62) & (v > 0.55)
linen = (s < 0.22) & (v > 0.62)
metal = (rm0[..., 2] > 0.45) & (s < 0.35) & (v > 0.25)
cls[green] = CLOTH
cls[hair] = HAIR
cls[skin] = SKIN
cls[linen] = LINEN
cls[metal] = METAL
# Clean speckle with a mode filter
cls = np.asarray(Image.fromarray(cls).filter(ImageFilter.ModeFilter(5))).copy()
# Shading painted into the face (under the eyes, the cheeks, the brows) reads as leather by colour; small
# non-skin islands that skin mostly surrounds are skin.
from scipy import ndimage
for c in (LEATHER, LINEN, HAIR):
    lab, n = ndimage.label(cls == c)
    if n == 0: continue
    area = ndimage.sum(np.ones_like(lab), lab, np.arange(1, n + 1))
    for i in np.nonzero(area < 20000)[0] + 1:
        m = lab == i
        ring = ndimage.binary_dilation(m, iterations=3) & ~m
        if ring.any() and (cls[ring] == SKIN).mean() > 0.6:
            cls[m] = SKIN

#                 none  cloth linen skin  hair leather metal
rough = np.array([0.8, 0.88, 0.8, 0.5, 0.42, 0.55, 0.28], np.float32)
metalv = np.array([0, 0, 0, 0, 0, 0, 0.95], np.float32)
R = rough[cls]
# keep a little of the original roughness detail inside each class
R = np.clip(R + (rm0[..., 1] - rm0[..., 1].mean()) * 0.15, 0.05, 1.0)
M = metalv[cls]
R = np.asarray(Image.fromarray((R * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2))).astype(np.float32) / 255
M = np.asarray(Image.fromarray((M * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(1.2))).astype(np.float32) / 255
rmout = np.zeros((H, W, 4), np.uint8); rmout[..., 0] = 255; rmout[..., 1] = R * 255; rmout[..., 2] = M * 255; rmout[..., 3] = 255
Image.fromarray(rmout).save(f'{outdir}/rm.png')
sk = (cls == SKIN).astype(np.uint8) * 255
sk = np.asarray(Image.fromarray(sk).filter(ImageFilter.GaussianBlur(1.5)))
skout = np.zeros((H, W, 4), np.uint8); skout[..., 0] = sk; skout[..., 3] = 255
Image.fromarray(skout).save(f'{outdir}/skinmask.png')
np.save(f'{outdir}/classes.npy', cls)
pal = np.array([[0, 0, 0], [40, 200, 40], [230, 230, 200], [255, 170, 130], [255, 230, 40], [120, 70, 30], [160, 180, 255]], np.uint8)
prev = Image.fromarray(pal[cls]).resize((512, 512), Image.NEAREST)
o = Image.new('RGB', (1024, 512)); o.paste(Image.fromarray((base * 255).astype(np.uint8)).resize((512, 512)), (0, 0)); o.paste(prev, (512, 0)); o.save(f'{outdir}/classes_preview.png')
print({n: int((cls == i).sum()) for i, n in enumerate(['none', 'cloth', 'linen', 'skin', 'hair', 'leather', 'metal'])})
