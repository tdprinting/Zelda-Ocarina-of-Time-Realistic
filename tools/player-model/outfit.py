# Repaints young Link's outfit to match his original clothes, and builds the material maps from the same regions.
#
# The source model came with its own costume painted in (a braided strap, a white sash, embroidered dark panels,
# a pouch, fur-topped boots), and the material maps were guessed from colour alone, so the shading looked random
# against what Link actually wears. Here every texel is located on the body (rasterising the mesh in UV space)
# and assigned to a part of the original outfit: green tunic and hat, red-brown belt with a gold buckle, brown
# boots with lighter tops, bare skin, blond hair. Each part gets one albedo; only the fine detail of the source
# (weave, stitching, folds) is kept. Roughness/metal, the skin mask and the wet/dirt classes come from the same
# regions, so every part shades as the material it is.
#
# It also repairs the mesh: triangles whose UVs stretch across the atlas (left by hole filling) get their own
# vertices on a nearby patch of the right colour, and small loose pieces held in the hands are removed.
#
# usage: outfit.py model.npz basecolor.png normal.png outdir
#   writes outdir/{model.npz, basecolor.png, normal.png, rm.png, skinmask.png, classes.npy, preview.png}
import sys, numpy as np
from PIL import Image, ImageFilter
from scipy import ndimage
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components

npz_in, base_in, normal_in, outdir = sys.argv[1:5]
D = dict(np.load(npz_in, allow_pickle=True))
names = [str(n) for n in D['names']]
B = {n: i for i, n in enumerate(names)}
n_main = int(D['mat_ranges'][0][1])

# ---------------------------------------------------------------------------------------------------- mesh repairs
P, UV, T, BI = D['pos'], D['uv'].copy(), D['tris'].reshape(-1, 3).copy(), D['bi']
TM = T[:n_main]

# Loose pieces: small connected parts on the hands and skirt (sticks and trinkets). Eyes, lids and teeth stay.
key = np.unique(np.round(P, 1), axis=0, return_inverse=True)[1].ravel()
e = np.concatenate([TM[:, [0, 1]], TM[:, [1, 2]], TM[:, [2, 0]]])
m = key.max() + 1
nc, lab = connected_components(coo_matrix((np.ones(len(e)), (key[e[:, 0]], key[e[:, 1]])), shape=(m, m)), directed=False)
tl = lab[key[TM[:, 0]]]
size = np.bincount(tl, minlength=nc)
loose_bones = {B['LeftHand'], B['RightHand']} | {i for n, i in B.items() if n.startswith('Skirt')}
drop = np.zeros(len(TM), bool)
for c in np.nonzero((size > 0) & (size <= 12))[0]:
    tris = np.nonzero(tl == c)[0]
    if np.bincount(BI[TM[tris].ravel(), 0]).argmax() in loose_bones:
        drop[tris] = True

# Stretched UVs: any edge longer than 60 texels of the 1024 atlas (typical edges are 6).
edge = np.stack([np.linalg.norm((UV[TM[:, i]] - UV[TM[:, (i + 1) % 3]]) * 1024, axis=1) for i in range(3)], 1).max(1)
bad = (edge > 60) & ~drop
good_v = np.unique(TM[~bad & ~drop].ravel())
tree_pts = P[good_v]
new_pos, new_nrm, new_uv, new_bi, new_bw = [], [], [], [], []
nv = len(P)
for ti in np.nonzero(bad)[0]:
    c = P[TM[ti]].mean(0)
    j = good_v[np.argmin(((tree_pts - c) ** 2).sum(1))]
    for k in range(3):
        v = TM[ti, k]
        new_pos.append(P[v]); new_nrm.append(D['nrm'][v]); new_bi.append(BI[v]); new_bw.append(D['bw'][v])
        new_uv.append(UV[j])
        TM[ti, k] = nv; nv += 1
if new_pos:
    D['pos'] = np.concatenate([P, new_pos]); D['nrm'] = np.concatenate([D['nrm'], new_nrm])
    D['uv'] = np.concatenate([UV, new_uv]); D['bi'] = np.concatenate([BI, new_bi]); D['bw'] = np.concatenate([D['bw'], new_bw])
T[:n_main] = TM
keep = np.ones(len(T), bool); keep[:n_main][drop] = False
D['tris'] = T[keep]
D['mat_ranges'] = np.array([[0, n_main - drop.sum()], [n_main - drop.sum(), len(T) - n_main]])
print(f'removed {drop.sum()} loose triangles, re-mapped {bad.sum()} stretched ones')
P, UV, T, BI = D['pos'], D['uv'], D['tris'], D['bi']
TM = T[:int(D['mat_ranges'][0][1])]

# ------------------------------------------------------------------------------------- texel -> body position/bone
N = 1024
base = np.asarray(Image.open(base_in).convert('RGB')).astype(np.float32) / 255
assert base.shape[:2] == (N, N)
pos = np.full((N, N, 3), np.nan, np.float32); bone = np.full((N, N), 255, np.uint8)
uvp = np.stack([UV[:, 0] * N, (1 - UV[:, 1]) * N], 1)
for t in TM:
    a, b, c = uvp[t]
    x0, x1 = int(max(0, np.floor(min(a[0], b[0], c[0])))), int(min(N - 1, np.ceil(max(a[0], b[0], c[0]))))
    y0, y1 = int(max(0, np.floor(min(a[1], b[1], c[1])))), int(min(N - 1, np.ceil(max(a[1], b[1], c[1]))))
    if x1 < x0 or y1 < y0: continue
    xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(den) < 1e-12: continue
    w0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / den
    w1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / den
    W = np.stack([w0, w1, 1 - w0 - w1], -1)
    msk = (W >= -0.02).all(-1)
    if not msk.any(): continue
    sub = (slice(y0, y1 + 1), slice(x0, x1 + 1))
    pos[sub][msk] = (W @ P[t])[msk]
    bone[sub][msk] = BI[t[np.argmax(W, -1)], 0][msk]
covered = bone < 255

# ------------------------------------------------------------------------------------------- colour classes
mx = base.max(2); mn = base.min(2); s = np.where(mx > 1e-4, (mx - mn) / np.maximum(mx, 1e-4), 0)
r, g, b = base[..., 0], base[..., 1], base[..., 2]; d = np.maximum(mx - mn, 1e-4)
h = np.where(mx == r, ((g - b) / d) % 6, np.where(mx == g, (b - r) / d + 2, (r - g) / d + 4)) / 6
h = np.where(mx - mn < 1e-4, 0, h)
is_skin = (h > 0.0) & (h <= 0.09) & (s > 0.15) & (s < 0.62) & (mx > 0.55)
is_hair = (h > 0.09) & (h <= 0.17) & (s > 0.35) & (mx > 0.45)
is_skin = ndimage.binary_opening(is_skin, iterations=1); is_hair = ndimage.binary_opening(is_hair, iterations=1)

# ------------------------------------------------------------------------------------------------- regions
TUNIC, HAT, BELT, BUCKLE, BOOT, BOOT_TOP, SOLE, SKIN, HAIR, FACE = range(1, 11)
group = lambda *ns: np.isin(bone, [B[n] for n in ns])
skirt = np.isin(bone, [i for n, i in B.items() if n.startswith('Skirt')])
torso = group('Root', 'Waist', 'LowerControl', 'UpperControl', 'Collar', 'Torso', 'Sheath') | skirt
arms = group('LeftShoulder', 'RightShoulder')
lower_arms = group('LeftForearm', 'RightForearm', 'LeftHand', 'RightHand')
hatb = group('Hat', 'HatCloth0', 'HatCloth1', 'HatCloth2', 'HatCloth3')
head = group('Head')
thighs, shins, feet = group('LeftThigh', 'RightThigh'), group('LeftShin', 'RightShin'), group('LeftFoot', 'RightFoot')
y = np.nan_to_num(pos[..., 1]); z = np.nan_to_num(pos[..., 2]); x = np.nan_to_num(pos[..., 0])

# The belt: the leather band around the waist, and its buckle (the metal ring on the front).
BELT_LO, BELT_HI = 2225.0, 2425.0
BUCKLE_X, BUCKLE_Y, BUCKLE_R = -50.0, 2320.0, 115.0
is_green = (h > 0.17) & (h < 0.45) & (s > 0.15)
# Skin in shadow fails the strict skin test; on the limbs anything warm and not dark is skin.
is_warm = ((h < 0.12) | (h > 0.92)) & (s > 0.1) & (mx > 0.35)
is_dark = (mx < 0.4) & ~is_green
reg = np.zeros((N, N), np.uint8)
# Torso: green cloth is the tunic; warm light texels are skin (neck, chest opening); the rest of the source
# costume (strap, sash, pouch, embroidery) becomes tunic too.
reg[torso] = TUNIC
reg[torso & is_skin & (y > 2900)] = SKIN
belt = torso & (y > BELT_LO) & (y < BELT_HI) & ~is_green
reg[belt] = BELT
reg[torso & (z > 0) & ((x - BUCKLE_X) ** 2 + (y - BUCKLE_Y) ** 2 < BUCKLE_R ** 2) & (s < 0.3) & (mx > 0.3)] = BUCKLE
# Arms and legs: green is the sleeve or the tunic hem, everything else is bare skin (Link's arms and hands are bare).
reg[arms | thighs | lower_arms] = SKIN
reg[(arms | thighs) & is_green] = TUNIC
# Boots: skin above the boot, leather below, with the lighter folded top.
reg[shins] = BOOT
reg[shins & (y > 640)] = BOOT_TOP
reg[shins & (y > 820)] = SKIN
reg[feet] = BOOT
reg[feet & (y < 70)] = SOLE
# Head and hat: keep the painted face and hair; green cloth and the dark band are the hat.
reg[head | hatb] = FACE
reg[(head | hatb) & is_hair] = HAIR
reg[(head | hatb) & is_green] = HAT
reg[(head & (y > 4230) | hatb) & is_dark & ~is_hair] = HAT
reg[hatb & ~is_hair] = HAT
reg[head & (y > 4620) & ~is_hair] = HAT  # the crown, above the fringe
reg[~covered] = 0
# Fill speckle inside each part.
reg = np.where(covered, np.asarray(Image.fromarray(reg).filter(ImageFilter.ModeFilter(5))), 0).astype(np.uint8)

# ------------------------------------------------------------------------------------------------- repaint
# sRGB albedo per part (darker and less saturated than the N64 colours, as real dyed cloth and leather are).
ALBEDO = {TUNIC: (46, 104, 38), HAT: (46, 104, 38), BELT: (104, 44, 28), BUCKLE: (214, 172, 84), BOOT: (88, 56, 32),
          BOOT_TOP: (138, 98, 56), SOLE: (52, 36, 24)}
DETAIL = {TUNIC: 0.55, HAT: 0.55, BELT: 0.6, BUCKLE: 0.4, BOOT: 0.6, BOOT_TOP: 0.6, SOLE: 0.4}
lin = base ** 2.2
lum = lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)
out = base.copy()
for k, col in ALBEDO.items():
    msk = reg == k
    if not msk.any(): continue
    mf = msk.astype(np.float32)
    low = ndimage.gaussian_filter(lum * mf, 6) / np.maximum(ndimage.gaussian_filter(mf, 6), 1e-4)
    ratio = np.clip(lum / np.maximum(low, 1e-4), 0.4, 1.8) ** DETAIL[k]
    a = (np.array(col, np.float32) / 255) ** 2.2
    out[msk] = np.clip(a * ratio[msk, None], 0, 1) ** (1 / 2.2)
# Body skin: one smooth tone taken from the painted skin around it, with the source's fine detail. Texels that were
# costume before (wristbands, the sash, shorts) blend in instead of showing as pale patches. The face keeps its paint.
body_skin = reg == SKIN
true_skin = (body_skin | (reg == FACE)) & is_skin
tf = true_skin.astype(np.float32)
tone = np.stack([ndimage.gaussian_filter(lin[..., k] * tf, 24) for k in range(3)], -1) / np.maximum(ndimage.gaussian_filter(tf, 24), 1e-4)[..., None]
tone = np.where(ndimage.gaussian_filter(tf, 24)[..., None] > 0.02, tone, (lin[true_skin]).mean(0))
mf = body_skin.astype(np.float32)
low = ndimage.gaussian_filter(lum * mf, 6) / np.maximum(ndimage.gaussian_filter(mf, 6), 1e-4)
ratio = np.clip(lum / np.maximum(low, 1e-4), 0.6, 1.4) ** 0.5
out[body_skin] = np.clip(tone[body_skin] * ratio[body_skin, None], 0, 1) ** (1 / 2.2)

# Bleed every part's colour into the gaps between UV charts so filtering never picks up a neighbour.
idx = ndimage.distance_transform_edt(~covered, return_distances=False, return_indices=True)
out = out[idx[0], idx[1]]
reg_f = reg[idx[0], idx[1]]

# Normal map: soften the embossing of the source costume (strap braid, embroidery) on the cloth.
nrm = np.asarray(Image.open(normal_in).convert('RGB')).astype(np.float32) / 255 * 2 - 1
cloth = np.isin(reg_f, [TUNIC, HAT])[..., None]
soft = nrm * np.array([0.45, 0.45, 1.0], np.float32)
soft /= np.linalg.norm(soft, axis=-1, keepdims=True)
nrm = np.where(cloth, soft, nrm)

# ------------------------------------------------------------------------------------------- material maps
#                     tunic hat  belt buckle boot top  sole skin hair face
ROUGH = np.array([0.8, 0.86, 0.86, 0.5, 0.3, 0.55, 0.62, 0.7, 0.5, 0.42, 0.5], np.float32)
METAL = np.array([0, 0, 0, 0, 0.95, 0, 0, 0, 0, 0, 0], np.float32)
# Surface classes for wetness and dirt (SurfaceState): 1 cloth, 3 skin, 4 hair, 5 leather, 6 metal.
SURF = np.array([0, 1, 1, 5, 6, 5, 5, 5, 3, 4, 3], np.uint8)
face_skin = (reg_f == FACE) & is_skin[idx[0], idx[1]]
rough = ROUGH[reg_f]; rough[(reg_f == FACE) & ~face_skin] = 0.55
rough = ndimage.gaussian_filter(rough, 1.0); metal = ndimage.gaussian_filter(METAL[reg_f], 1.0)
classes = SURF[reg_f]; classes[(reg_f == FACE) & ~face_skin] = 5
classes[~covered] = 0
skin = ndimage.gaussian_filter(((reg_f == SKIN) | face_skin).astype(np.float32), 1.5)

def save(arr, name):
    Image.fromarray(np.clip(arr * 255 + 0.5, 0, 255).astype(np.uint8)).save(f'{outdir}/{name}')
save(np.dstack([out, np.ones((N, N))]), 'basecolor.png')
save(np.dstack([nrm * 0.5 + 0.5, np.ones((N, N))]), 'normal.png')
save(np.dstack([np.ones((N, N)), rough, metal, np.ones((N, N))]), 'rm.png')
save(np.dstack([skin, np.zeros((N, N)), np.zeros((N, N)), np.ones((N, N))]), 'skinmask.png')
np.save(f'{outdir}/classes.npy', classes)
np.savez(f'{outdir}/model.npz', **D)
pal = np.array([[0, 0, 0], [40, 160, 40], [90, 220, 90], [150, 40, 30], [255, 210, 60], [110, 70, 40], [180, 140, 90],
                [40, 30, 20], [255, 180, 150], [255, 230, 80], [255, 140, 200]], np.uint8)
prev = Image.new('RGB', (1024, 512))
prev.paste(Image.fromarray(pal[reg]).resize((512, 512), Image.NEAREST), (0, 0))
prev.paste(Image.fromarray((out * 255).astype(np.uint8)).resize((512, 512)), (512, 0))
prev.save(f'{outdir}/preview.png')
print({n: int((reg == i).sum()) for i, n in enumerate(['none', 'tunic', 'hat', 'belt', 'buckle', 'boot', 'boot_top',
                                                       'sole', 'skin', 'hair', 'face'])})
