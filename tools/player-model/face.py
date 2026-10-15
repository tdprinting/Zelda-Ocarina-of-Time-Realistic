import os
# Face pass on the exported player model (skeleton units, +Z = front, +X = Link's left, +Y = up).
#  1. Straightens the head (the source sculpt looks 20 degrees to his right) and centres the face.
#  2. Refines the face mesh, cuts the painted eyes out and puts real eyeballs (own bones and
#     texture) in sockets behind the lids.
#  3. Cuts a mouth slit with a dark mouth cavity behind it.
#  4. Authors the expression blendshapes (blinks, squints, brows, smile, frown, jaw...).
# usage: face.py in.npz out.npz
import sys, json, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from meshops import subdivide, boundary_loops
src, out = sys.argv[1], sys.argv[2]
D = dict(np.load(src, allow_pickle=True))
s = 5000.0
names = [str(n) for n in D['names']]
binds = D['binds'].copy(); parents = list(D['parents'])
HEAD = names.index('Head')

# ---------------------------------------------------------------- 1. straighten the head
P, N, BI, BW = D['pos'].copy(), D['nrm'].copy(), D['bi'], D['bw']
def b2s(b): b = np.asarray(b, float); return s * np.array([b[0], b[2], -b[1]])
eyeR0 = b2s([-0.0697, -0.1126, 0.8034]); eyeL0 = b2s([0.0063, -0.1399, 0.803])
v = eyeL0 - eyeR0
ang = np.arctan2(v[2], v[0])
c, sn = np.cos(ang), np.sin(ang)
R = np.array([[c, 0, sn], [0, 1, 0], [-sn, 0, c]])
pivot = binds[HEAD][:3, 3].copy()
SHIFT = np.array([30.0, 0, 0])   # measured face centre was 30 units right of the body midline
headset = [names.index(n) for n in names if n in ('Head', 'Hat') or n.startswith('HatCloth')]
w = np.zeros(len(P))
for k in range(4):
    w += BW[:, k] * np.isin(BI[:, k], headset)
moved = (P - pivot) @ R.T + pivot + SHIFT
P = P + w[:, None] * (moved - P)
Nr = N @ R.T
N = N + w[:, None] * (Nr - N); N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-8)
T4 = np.eye(4); T4[:3, :3] = R; T4[:3, 3] = pivot - R @ pivot + SHIFT
for i, n in enumerate(names):
    if n == 'Hat' or n.startswith('HatCloth'):
        binds[i] = T4 @ binds[i]
M = dict(pos=P, nrm=N, uv=D['uv'].copy(), tris=D['tris'].astype(np.int64), bi=BI.astype(np.int64), bw=BW.copy())

def headw(M):
    w = np.zeros(len(M['pos']))
    for k in range(4):
        w += M['bw'][:, k] * (M['bi'][:, k] == HEAD)
    return w

# ---------------------------------------------------------------- face landmarks (after straightening)
EYE_Y = 4034.0
EYES = {'R': np.array([-205.0, EYE_Y]), 'L': np.array([205.0, EYE_Y])}
EYE_RX, EYE_RY = 90.0, 64.0
MOUTH_X, MOUTH_Y, MOUTH_HW = 13.0, 3755.0, 112.0
BROWS = {'R': np.array([-215.0, 4150.0]), 'L': np.array([215.0, 4150.0])}

# ---------------------------------------------------------------- 2. refine the face
for it in range(2):
    C = M['pos'][M['tris']].mean(1)
    hw_t = headw(M)[M['tris']].mean(1)
    mask = (C[:, 1] > 3560) & (C[:, 1] < 4260) & (C[:, 2] > 380) & (np.abs(C[:, 0]) < 400) & (hw_t > 0.5)
    print('subdivide pass', it, 'tris', mask.sum())
    M = subdivide(M, mask)

# ---------------------------------------------------------------- 2b. cut eyes, add sockets + eyeballs
EYE_R = 98.0
eye_info = {}
C = M['pos'][M['tris']].mean(1)
keep = np.ones(len(M['tris']), bool)
for side, e in EYES.items():
    inside = (((C[:, 0] - e[0]) / EYE_RX) ** 2 + ((C[:, 1] - e[1]) / EYE_RY) ** 2 < 1) & (C[:, 2] > 450)
    keep &= ~inside
    print('eye', side, 'cut tris', inside.sum())
mouth_cut = ((((C[:, 0] - MOUTH_X) / (MOUTH_HW * 0.93)) ** 2 + ((C[:, 1] - MOUTH_Y) / 7.0) ** 2) < 1) & (C[:, 2] > 450)
print('mouth cut tris', mouth_cut.sum())
keep &= ~mouth_cut
M['tris'] = M['tris'][keep]

loops, rep = boundary_loops(M['tris'], len(M['pos']), M['pos'])
def loop_near(cx, cy, maxd):
    best = None
    for lp in loops:
        q = M['pos'][lp]
        d = np.hypot(q[:, 0].mean() - cx, q[:, 1].mean() - cy)
        if d < maxd and (best is None or d < best[0]) and len(lp) >= 6:
            best = (d, lp)
    return best[1] if best else None

def add_verts(pos, nrm, uv, bi, bw):
    base = len(M['pos'])
    M['pos'] = np.vstack([M['pos'], pos]); M['nrm'] = np.vstack([M['nrm'], nrm]); M['uv'] = np.vstack([M['uv'], uv])
    M['bi'] = np.vstack([M['bi'], bi]); M['bw'] = np.vstack([M['bw'], bw])
    return np.arange(base, base + len(pos))

def add_tris(t):
    M['tris'] = np.vstack([M['tris'], np.asarray(t, dtype=np.int64).reshape(-1, 3)])

headBI = np.array([HEAD, 0, 0, 0]); headBW = np.array([1.0, 0, 0, 0])
eye_bones = {}
eye_tris = []
socket_inner = {}
for side, e in EYES.items():
    lp = loop_near(e[0], e[1], 40)
    assert lp is not None, 'eye loop not found ' + side
    q = M['pos'][lp]
    zb = q[:, 2].mean()
    centre = np.array([e[0], e[1], zb + 4.0 - EYE_R])
    eye_info[side] = dict(loop=lp, centre=centre)
    # socket rim: boundary pulled into the eyeball so no gap shows around it
    inner = []
    for v_ in lp:
        p = M['pos'][v_]; d = p - centre; d /= np.linalg.norm(d)
        inner.append(centre + d * EYE_R * 0.86)
    inner = np.array(inner)
    ids = add_verts(inner, M['nrm'][lp], M['uv'][lp], M['bi'][lp], M['bw'][lp])
    socket_inner[side] = ids
    n = len(lp)
    for k in range(n):
        a, b = lp[k], lp[(k + 1) % n]; ia, ib = ids[k], ids[(k + 1) % n]
        add_tris([[b, a, ia], [b, ia, ib]])
    bname = 'Eye' + side
    B = np.eye(4); B[:3, 3] = centre
    names.append(bname); binds = np.concatenate([binds, B[None]]); parents.append(HEAD)
    eye_bones[side] = len(names) - 1

# eyeball meshes (UV sphere, front-projected UVs into the eye texture)
for side in ('R', 'L'):
    centre = eye_info[side]['centre']; bone = eye_bones[side]
    nlat, nlon = 18, 28
    verts, nrms, uvs = [], [], []
    for i in range(nlat + 1):
        th = np.pi * i / nlat          # 0 = front pole (+Z)
        for j in range(nlon):
            ph = 2 * np.pi * j / nlon
            d = np.array([np.sin(th) * np.cos(ph), np.sin(th) * np.sin(ph), np.cos(th)])
            verts.append(centre + d * EYE_R); nrms.append(d)
            r = np.sin(th) if th < np.pi / 2 else 1.0
            uvs.append([0.5 + 0.5 * r * np.cos(ph) * 0.98, 0.5 + 0.5 * r * np.sin(ph) * 0.98])
    ids = add_verts(np.array(verts), np.array(nrms), np.array(uvs),
                    np.tile([bone, 0, 0, 0], (len(verts), 1)), np.tile([1.0, 0, 0, 0], (len(verts), 1)))
    for i in range(nlat):
        for j in range(nlon):
            a = ids[i * nlon + j]; b = ids[i * nlon + (j + 1) % nlon]
            c_ = ids[(i + 1) * nlon + j]; d_ = ids[(i + 1) * nlon + (j + 1) % nlon]
            if i == 0: eye_tris.append([a, c_, d_])
            elif i == nlat - 1: eye_tris.append([a, c_, b])
            else: eye_tris += [[a, c_, d_], [a, d_, b]]
    eye_info[side]['ids'] = ids

# eyelids: a skin-coloured shell around each eyeball on its own bone. Open, it sits tucked up
# inside the head behind the brow; blinking rotates it down over the eye.
LID_R = EYE_R * 1.035
LID_OPEN_EDGE = np.radians(26)      # elevation of the lid edge when open (covers the top of the iris a little)
lid_bones = {}
for side in ('R', 'L'):
    centre = eye_info[side]['centre']
    # skin colour: UV of the face vertex just above the upper lid, lash colour: darkest of the lid edge
    lp = eye_info[side]['loop']; q = M['pos'][lp]
    top = lp[int(np.argmax(q[:, 1]))]
    cand = np.nonzero((np.abs(M['pos'][:, 0] - centre[0]) < 25) & (M['pos'][:, 1] < q[:, 1].min() - 70) &
                      (M['pos'][:, 1] > q[:, 1].min() - 120) & (M['pos'][:, 2] > centre[2]))[0]
    skin_uv = M['uv'][cand[0]] if len(cand) else M['uv'][top]
    lash_uv = M['uv'][top]
    bname = 'Lid' + side
    B = np.eye(4); B[:3, 3] = centre
    names.append(bname); binds = np.concatenate([binds, B[None]]); parents.append(HEAD)
    lid_bones[side] = len(names) - 1
    ne, na = 8, 14
    els = np.linspace(LID_OPEN_EDGE, np.radians(115), ne)
    azs = np.linspace(-np.radians(72), np.radians(72), na)
    verts, nrms, uvs = [], [], []
    for i, el in enumerate(els):
        for az in azs:
            d = np.array([np.cos(el) * np.sin(az), np.sin(el), np.cos(el) * np.cos(az)])
            # thicker edge: the first row sits a little further out so the lid has a rim
            rr = LID_R + (4.0 if i == 0 else 0.0)
            verts.append(centre + d * rr); nrms.append(d)
            uvs.append(lash_uv if i == 0 else skin_uv)
    ids = add_verts(np.array(verts), np.array(nrms), np.array(uvs),
                    np.tile([lid_bones[side], 0, 0, 0], (len(verts), 1)), np.tile([1.0, 0, 0, 0], (len(verts), 1)))
    for i in range(ne - 1):
        for j in range(na - 1):
            a = ids[i * na + j]; b = ids[i * na + j + 1]; c_ = ids[(i + 1) * na + j]; d_ = ids[(i + 1) * na + j + 1]
            add_tris([[a, b, d_], [a, d_, c_]])
    eye_info[side]['lid'] = ids

# ---------------------------------------------------------------- 3. mouth cavity
DARK_UV = np.array([992.5 / 1024, 1 - 4.5 / 1024])
mlp = loop_near(MOUTH_X, MOUTH_Y, 60)
assert mlp is not None, 'mouth loop not found'
mq = M['pos'][mlp]
for v_ in mlp:
    p = M['pos'][v_]
    t = np.clip((p[0] - MOUTH_X) / MOUTH_HW, -1, 1)
    half = 3.0 * (1 - t * t)                     # lips meet at the corners
    p[1] = MOUTH_Y + (half if p[1] >= MOUTH_Y else -half)
for v_ in mlp:
    same = np.nonzero(rep == rep[v_])[0]
    M['pos'][same] = M['pos'][v_]
mq = M['pos'][mlp]
mouth_z = mq[:, 2].mean()
ring = []
for v_ in mlp:
    p = M['pos'][v_].copy()
    p[2] -= 45; p[1] = MOUTH_Y + (p[1] - MOUTH_Y) * 3.0
    ring.append(p)
ring = np.array(ring)
nr = len(mlp)
ring_ids = add_verts(ring, np.tile([0, 0, 1.0], (nr, 1)), np.tile(DARK_UV, (nr, 1)), M['bi'][mlp], M['bw'][mlp])
lip_dup = add_verts(M['pos'][mlp], M['nrm'][mlp], np.tile(DARK_UV, (nr, 1)), M['bi'][mlp], M['bw'][mlp])
for k in range(nr):
    a, b = lip_dup[k], lip_dup[(k + 1) % nr]; ia, ib = ring_ids[k], ring_ids[(k + 1) % nr]
    add_tris([[b, a, ia], [b, ia, ib]])
mouth_ring = np.concatenate([ring_ids, lip_dup])
WHITE_UV = np.array([118.5 / 1024, 1 - 641.5 / 1024])
teeth = {}
for name_, y0, y1, zoff in (('upper', MOUTH_Y + 6, MOUTH_Y - 9, -16), ('lower', MOUTH_Y - 16, MOUTH_Y - 4, -22)):
    xs_ = np.linspace(MOUTH_X - MOUTH_HW * 0.62, MOUTH_X + MOUTH_HW * 0.62, 9)
    vv, nn = [], []
    for x_ in xs_:
        curve = 22 * ((x_ - MOUTH_X) / (MOUTH_HW * 0.62)) ** 2   # teeth follow the jaw arch
        for y_ in (y0, y1):
            vv.append([x_, y_, mouth_z + zoff - curve]); nn.append([0, 0, 1.0])
    ids = add_verts(np.array(vv), np.array(nn), np.tile(WHITE_UV, (len(vv), 1)), np.tile(headBI, (len(vv), 1)), np.tile(headBW, (len(vv), 1)))
    for i in range(len(xs_) - 1):
        a, b, c_, d_ = ids[2 * i], ids[2 * i + 1], ids[2 * i + 2], ids[2 * i + 3]
        add_tris([[a, b, d_], [a, d_, c_]] if name_ == 'upper' else [[a, d_, b], [a, c_, d_]])
    teeth[name_] = ids
back = np.array([MOUTH_X, MOUTH_Y - 10, mouth_z - 160])
back_id = add_verts(back[None], np.array([[0, 0, 1.0]]), DARK_UV[None], headBI[None], headBW[None])[0]
for k in range(nr):
    add_tris([[ring_ids[(k + 1) % nr], ring_ids[k], back_id]])

# ---------------------------------------------------------------- 4. blendshapes
P = M['pos']; NV = len(P)
hw = headw(M)
face = (hw > 0.3) & (P[:, 2] > 300) & (P[:, 1] > 3450)
eyeball = np.zeros(NV, bool)
for side in eye_info:
    eyeball[eye_info[side]['ids']] = True
    eyeball[eye_info[side]['lid']] = True
face &= ~eyeball
morphs = {}

def gauss(cx, cy, sx, sy):
    return np.exp(-(((P[:, 0] - cx) / sx) ** 2 + ((P[:, 1] - cy) / sy) ** 2) / 2) * face

def sphere_front_z(centre, x, y, margin):
    dx, dy = x - centre[0], y - centre[1]
    r2 = np.maximum(EYE_R ** 2 - dx * dx - dy * dy, 0)
    return centre[2] + np.sqrt(r2) + margin

for side in ('R', 'L'):
    info = eye_info[side]; centre = info['centre']; lp = np.array(info['loop']); inner = socket_inner[side]
    q = P[lp]
    upper = q[:, 1] > centre[1]
    lo = q[~upper]; lo = lo[np.argsort(lo[:, 0])]
    hi = q[upper]; hi = hi[np.argsort(hi[:, 0])]
    def lower_y(x): return np.interp(x, lo[:, 0], lo[:, 1])
    def upper_y(x): return np.interp(x, hi[:, 0], hi[:, 1])
    region = (np.abs(P[:, 0] - centre[0]) < EYE_RX * 1.9) & (np.abs(P[:, 1] - centre[1]) < EYE_RY * 2.6) & (face | np.isin(np.arange(NV), inner))
    idx = np.nonzero(region)[0]
    x, y = P[idx, 0], P[idx, 1]
    ly, uy = lower_y(x), upper_y(x)
    close_y = uy - 0.22 * (uy - ly)     # skin fold follows the lid a little
    du = np.clip((y - uy) / 70.0, 0, 1)
    dl = np.clip((ly - y) / 45.0, 0, 1)
    up_side = y >= (ly + uy) / 2
    fu = np.where(up_side, (1 - du) ** 2, 0)
    fl = np.where(~up_side, (1 - dl) ** 2, 0)
    dlt = np.zeros((NV, 3))
    dlt[idx, 1] = (close_y - uy) * fu + 0.12 * (uy - ly) * fl
    newp = P[idx] + dlt[idx]
    fz = sphere_front_z(centre, newp[:, 0], newp[:, 1], 9.0)
    dlt[idx, 2] = np.maximum(fz - newp[:, 2], 0) * np.maximum(fu, fl)
    morphs['blink' + side] = dlt
    d2 = np.zeros((NV, 3))
    d2[idx, 1] = np.where(up_side, -0.12 * (uy - ly) * (1 - du) ** 2, 0.4 * (uy - ly) * (1 - dl) ** 2 * (1 - 0.5 * dl))
    newp = P[idx] + d2[idx]
    d2[idx, 2] = np.maximum(sphere_front_z(centre, newp[:, 0], newp[:, 1], 9.0) - newp[:, 2], 0) * (1 - dl)
    morphs['squint' + side] = d2
    d3 = np.zeros((NV, 3))
    d3[idx, 1] = np.where(up_side, 14.0 * (1 - du) ** 2, -5.0 * (1 - dl) ** 2)
    morphs['wide' + side] = d3

sgn = {'R': -1.0, 'L': 1.0}
for side in ('R', 'L'):
    bx, by = BROWS[side]; sg = sgn[side]
    d = np.zeros((NV, 3)); g = gauss(bx, by, 95, 50); d[:, 1] = 30 * g; morphs['browUp' + side] = d
    inner_x = bx - sg * 95
    d = np.zeros((NV, 3)); g = gauss(inner_x, by - 10, 70, 45); d[:, 1] = -26 * g; d[:, 0] = -sg * 12 * g; d[:, 2] = 8 * g
    morphs['browDown' + side] = d
    d = np.zeros((NV, 3)); g = gauss(inner_x, by, 65, 45); d[:, 1] = 28 * g; morphs['browInnerUp' + side] = d
    cx = MOUTH_X + sg * MOUTH_HW
    d = np.zeros((NV, 3)); g = gauss(cx, MOUTH_Y, 60, 50); d[:, 0] = sg * 16 * g; d[:, 1] = 34 * g; d[:, 2] = -10 * g
    gc = gauss(MOUTH_X + sg * 190, 3880, 75, 60); d[:, 1] += 14 * gc; d[:, 2] += 9 * gc
    morphs['smile' + side] = d
    d = np.zeros((NV, 3)); g = gauss(cx, MOUTH_Y - 8, 60, 50); d[:, 1] = -24 * g; d[:, 2] = 4 * g; morphs['frown' + side] = d
    d = np.zeros((NV, 3)); g = gauss(MOUTH_X + sg * 190, 3870, 80, 65); d[:, 1] = 14 * g; d[:, 2] = 10 * g
    morphs['cheek' + side] = d

hinge = np.array([MOUTH_X, 3870.0, 260.0]); theta = np.radians(17)
cth, sth = np.cos(theta), np.sin(theta)
rel = P - hinge
rot = np.stack([rel[:, 0], rel[:, 1] * cth - rel[:, 2] * sth, rel[:, 1] * sth + rel[:, 2] * cth], 1) + hinge
below = np.clip((MOUTH_Y - P[:, 1]) / 6.0, 0, 1)
xs = np.clip(1 - (np.abs(P[:, 0] - MOUTH_X) - 150) / 160.0, 0, 1)
neck = np.clip((P[:, 1] - 3420) / 140.0, 0, 1)
wj = below * xs * neck * (hw > 0.3) * (P[:, 2] > 150)
wj[eyeball] = 0
ring_low = P[ring_ids, 1] < MOUTH_Y
wj[ring_ids[ring_low]] = 1.0
wj[teeth['lower']] = 1.0
wj[teeth['upper']] = 0.0
morphs['jawOpen'] = (rot - P) * wj[:, None]
d = np.zeros((NV, 3)); g = gauss(MOUTH_X, MOUTH_Y, 75, 40)
d[:, 0] = -(P[:, 0] - MOUTH_X) * 0.28 * g; d[:, 2] = 14 * g
morphs['pucker'] = d

# ---------------------------------------------------------------- write
n_main = len(M['tris'])
M['tris'] = np.vstack([M['tris'], np.array(eye_tris, dtype=np.int64)])
materials = [('textures/player-model/child-link/basecolor', 0, n_main),
             ('textures/player-model/child-link/eye_basecolor', n_main, len(eye_tris))]
mnames = sorted(morphs)
mo = {}
for k in mnames:
    d = morphs[k]; nz = np.nonzero(np.abs(d).max(1) > 0.05)[0]
    mo[k] = (nz, d[nz])
    print('morph', k, len(nz), 'max', np.abs(d).max().round(1))
lid_meta = np.array([[LID_OPEN_EDGE, eye_info['R']['centre'][1]]])
np.savez(out, pos=M['pos'], nrm=M['nrm'], uv=M['uv'], tris=M['tris'], bi=M['bi'], bw=M['bw'], binds=binds,
         parents=np.array(parents), names=np.array(names), jointPos=D['jointPos'],
         mat_names=np.array([m[0] for m in materials]), mat_ranges=np.array([[m[1], m[2]] for m in materials]),
         morph_names=np.array(mnames), **{'morph_idx_' + k: mo[k][0] for k in mnames}, **{'morph_d_' + k: mo[k][1] for k in mnames},
         eye_centres=np.array([eye_info['R']['centre'], eye_info['L']['centre']]))
print('verts', len(M['pos']), 'tris', len(M['tris']), 'bones', len(names))
