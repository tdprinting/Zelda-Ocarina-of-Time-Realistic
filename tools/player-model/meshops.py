# Mesh helpers for the face pass, on dict models (pos, nrm, uv, tris, bi, bw).
import numpy as np

def _blend_weights(bi_list, bw_list, ws):
    acc = {}
    for bi, bw, w in zip(bi_list, bw_list, ws):
        for b, x in zip(bi, bw):
            if x > 0: acc[int(b)] = acc.get(int(b), 0) + x * w
    items = sorted(acc.items(), key=lambda t: -t[1])[:4]
    tot = sum(x for _, x in items) or 1
    bi = [b for b, _ in items] + [0] * (4 - len(items)); bw = [x / tot for _, x in items] + [0] * (4 - len(items))
    return bi, bw

def subdivide(M, tri_mask):
    """Splits the masked triangles 1->4 and fixes neighbours (red-green) so there are no cracks."""
    P, N, UV, T, BI, BW = M['pos'], M['nrm'], M['uv'], M['tris'], M['bi'], M['bw']
    newP, newN, newUV, newBI, newBW = list(P), list(N), list(UV), list(BI), list(BW)
    mid = {}
    def key(a, b): return (a, b) if a < b else (b, a)
    marked = set()
    for t in T[tri_mask]:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])): marked.add(key(a, b))
    def m(a, b):
        k = key(a, b)
        if k not in mid:
            mid[k] = len(newP)
            newP.append((P[a] + P[b]) / 2); n = N[a] + N[b]; newN.append(n / max(np.linalg.norm(n), 1e-8))
            newUV.append((UV[a] + UV[b]) / 2)
            bi, bw = _blend_weights([BI[a], BI[b]], [BW[a], BW[b]], [0.5, 0.5]); newBI.append(bi); newBW.append(bw)
        return mid[k]
    out = []
    for t in T:
        a, b, c = t
        e = [key(a, b) in marked, key(b, c) in marked, key(c, a) in marked]
        n = sum(e)
        if n == 0: out.append(t); continue
        if n == 3:
            ab, bc, ca = m(a, b), m(b, c), m(c, a)
            out += [[a, ab, ca], [ab, b, bc], [ca, bc, c], [ab, bc, ca]]; continue
        # rotate so the first marked edge is (v0,v1)
        vs = [a, b, c]; ee = e
        while not ee[0] or (n == 2 and ee[2]): vs = vs[1:] + vs[:1]; ee = ee[1:] + ee[:1]
        v0, v1, v2 = vs
        if n == 1:
            x = m(v0, v1); out += [[v0, x, v2], [x, v1, v2]]
        else:  # edges (v0,v1) and (v1,v2) marked
            x, y = m(v0, v1), m(v1, v2); out += [[x, v1, y], [v0, x, y], [v0, y, v2]]
    M.update(pos=np.array(newP), nrm=np.array(newN), uv=np.array(newUV), tris=np.array(out, dtype=np.int64),
             bi=np.array(newBI, dtype=np.int64), bw=np.array(newBW))
    return M

def tri_centroids(M): return M['pos'][M['tris']].mean(1)

def boundary_loops(T, nverts, P=None):
    """Boundary loops in welded (position) space: returns lists of vertex indices."""
    if P is not None:
        _, rep = np.unique(np.round(P, 3), axis=0, return_inverse=True); rep = rep.ravel()
    else:
        rep = np.arange(nverts)
    from collections import defaultdict
    cnt = defaultdict(int); orient = {}
    for t in T:
        for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
            ra, rb = rep[a], rep[b]; k = (min(ra, rb), max(ra, rb)); cnt[k] += 1; orient[(ra, rb)] = (a, b)
    nxt = {}
    for (ra, rb), (a, b) in orient.items():
        k = (min(ra, rb), max(ra, rb))
        if cnt[k] == 1: nxt[ra] = (rb, a)
    loops, seen = [], set()
    for st in list(nxt):
        if st in seen: continue
        lp = []; c = st
        while c in nxt and c not in seen:
            seen.add(c); rb, a = nxt[c]; lp.append(a); c = rb
        if c == st: loops.append(lp)
    return loops, rep
