# Writes the game's .lmdl (version 2) from a finished model npz (see PlayerModelData.h for the layout).
# usage: write_lmdl.py model.npz skel_fit.json out.lmdl
import sys, json, struct, numpy as np
npz, fitp, out = sys.argv[1:4]
D = dict(np.load(npz, allow_pickle=True)); F = json.load(open(fitp))
names = [str(n) for n in D['names']]; parents = list(D['parents']); binds = D['binds']
P, N, UV, T, BI, BW = D['pos'], D['nrm'], D['uv'], D['tris'], D['bi'], D['bw']
mnames = [str(n) for n in D.get('morph_names', [])]
def wstr(f, s_): b = s_.encode(); f.write(struct.pack('<I', len(b))); f.write(b)
with open(out, 'wb') as f:
    f.write(b'LMDL'); f.write(struct.pack('<I', 2))
    f.write(struct.pack('<IIII', len(P), len(T), len(names), len(mnames)))
    f.write(struct.pack('<f', F['ourHipHeight'] / F['ootHipHeight']))
    nl = len(F['names'])
    for i, b in enumerate(names):
        wstr(f, b); f.write(struct.pack('<i', int(parents[i])))
        f.write(struct.pack('<16f', *np.asarray(binds[i], np.float64).T.ravel()))
        jp = F['jointPos'][i] if i < nl else [0, 0, 0]
        f.write(struct.pack('<3f', *jp))
    q = np.zeros(len(P), dtype=[('p', '<f4', 3), ('n', '<i1', 3), ('pad', '<u1'), ('uv', '<f4', 2), ('bi', '<u1', 4), ('bw', '<u1', 4)])
    q['p'] = P; q['n'] = np.clip(np.round(N * 127), -127, 127); q['uv'] = UV; q['bi'] = BI
    bw8 = np.round(np.asarray(BW) * 255).astype(int); bw8[:, 0] += 255 - bw8.sum(1); q['bw'] = bw8
    f.write(q.tobytes())
    f.write(np.asarray(T).astype('<u4').tobytes())
    if 'mat_names' in D:
        f.write(struct.pack('<I', len(D['mat_names'])))
        for nm, (st, cnt) in zip(D['mat_names'], D['mat_ranges']):
            wstr(f, str(nm)); f.write(struct.pack('<II', int(st), int(cnt)))
    else:
        f.write(struct.pack('<I', 1)); wstr(f, 'textures/player-model/child-link/basecolor'); f.write(struct.pack('<II', 0, len(T)))
    for nm in mnames:
        idx = D['morph_idx_' + nm]; d = D['morph_d_' + nm]
        wstr(f, nm); f.write(struct.pack('<I', len(idx)))
        rec = np.zeros(len(idx), dtype=[('v', '<u4'), ('d', '<f4', 3)]); rec['v'] = idx; rec['d'] = d
        f.write(rec.tobytes())
    f.write(struct.pack('<I', 0))  # tags
print('wrote', out, len(P), 'verts', len(T), 'tris', len(names), 'bones', len(mnames), 'morphs')
