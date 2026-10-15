# Writes child-link.surf: a 256x256 UV-space table the game uses for wetness and dirt.
# Per texel: u8 surface class, u8 pad, u16 bind height (model units), i16 bind z (front +), i16 pad.
import sys, struct, numpy as np
from PIL import Image
npz, classes, out = sys.argv[1], sys.argv[2], sys.argv[3]
D = np.load(npz, allow_pickle=True)
P, UV, T = D['pos'], D['uv'], D['tris'].reshape(-1, 3)
N = 256
hy = np.full((N, N), -1.0, np.float32); hz = np.zeros((N, N), np.float32)
uvp = np.stack([UV[:, 0] * N, (1 - UV[:, 1]) * N], 1)  # image space, row 0 = top (v=1)
for t in T:
    a, b, c = uvp[t]
    x0, x1 = int(max(0, np.floor(min(a[0], b[0], c[0])))), int(min(N - 1, np.ceil(max(a[0], b[0], c[0]))))
    y0, y1 = int(max(0, np.floor(min(a[1], b[1], c[1])))), int(min(N - 1, np.ceil(max(a[1], b[1], c[1]))))
    if x1 < x0 or y1 < y0: continue
    xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
    den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
    if abs(den) < 1e-9: continue
    w0 = ((b[1] - c[1]) * (xs - c[0]) + (c[0] - b[0]) * (ys - c[1])) / den
    w1 = ((c[1] - a[1]) * (xs - c[0]) + (a[0] - c[0]) * (ys - c[1])) / den
    w2 = 1 - w0 - w1
    m = (w0 >= -0.05) & (w1 >= -0.05) & (w2 >= -0.05)
    if not m.any(): continue
    py = w0 * P[t[0], 1] + w1 * P[t[1], 1] + w2 * P[t[2], 1]
    pz = w0 * P[t[0], 2] + w1 * P[t[1], 2] + w2 * P[t[2], 2]
    sub = (slice(y0, y1 + 1), slice(x0, x1 + 1))
    hy[sub][m] = py[m]; hz[sub][m] = pz[m]
cls = np.load(classes)
cls = np.asarray(Image.fromarray(cls).resize((N, N), Image.NEAREST))
cls = np.where(hy < 0, 0, cls).astype(np.uint8)
hy = np.clip(hy, 0, 65535)
with open(out, 'wb') as f:
    f.write(b'LSRF'); f.write(struct.pack('<III', 1, N, N))
    rec = np.zeros(N * N, dtype=[('c', 'u1'), ('p', 'u1'), ('h', '<u2'), ('z', '<i2'), ('q', '<i2')])
    rec['c'] = cls.ravel(); rec['h'] = hy.ravel().astype(np.uint16); rec['z'] = np.clip(hz, -32768, 32767).ravel().astype(np.int16)
    f.write(rec.tobytes())
print('covered', (cls > 0).mean())
