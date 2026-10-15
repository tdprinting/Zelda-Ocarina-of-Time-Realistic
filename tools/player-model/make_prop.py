# Builds rigid prop models (adult Link's Master Sword and its sheath) from a source .glb that holds both side by
# side, standing upright (tip up, sword at -X, sheath at +X, decorated faces towards -Y).
# usage (Blender python): make_prop.py -- src.glb out_dir texture_prefix
# Writes <out_dir>/master-sword.lmdl, master-sheath.lmdl and the extracted base/normal/rm maps.
#
# Placement is in the limb spaces adult Link's vanilla display lists use (measured from object_link_boy):
#   hand:   gLinkAdultLeftHandHoldingMasterSwordNearDL - blade along +X, crossguard along Y, emblem towards -Z,
#           fist spans x -308..315, sword axis at y 328, z -76, crossguard centre x 365.
#   sheath: gLinkAdultMasterSwordAndSheathNearDL (Sheath limb) - sheath along +X from its mouth at x -387,
#           wide side along Y centred at y 18, z 0, outer face towards -Z; sheathed guard centre at x -349.
import bpy, sys, struct, numpy as np

src, out_dir, tex_prefix = sys.argv[sys.argv.index('--') + 1:][:3]
UNITS = 4600.0          # model units per metre: the hilt fills the fist and the blade is a little longer than vanilla
GRIP_TOP = 0.166        # source: where the crossguard starts (metres from the pommel end)
GUARD_HAND_X = 300.0    # hand space: crossguard sits flush against the top of the fist
HAND_AXIS = (328.0, -76.0)
SHEATH_MOUTH_X = -387.0
SHEATH_AXIS = (18.0, 0.0)
SHEATHED_GUARD_X = -400.0

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
obj = [o for o in bpy.data.objects if o.type == 'MESH'][0]
me = obj.data
me.calc_loop_triangles()
M = np.array(obj.matrix_world)
N3 = np.array(obj.matrix_world.to_3x3().inverted().transposed())
co = np.array([v.co for v in me.vertices]) @ M[:3, :3].T + M[:3, 3]
uvl = me.uv_layers.active.data
corner_normals = [cn.vector for cn in me.corner_normals] if hasattr(me, 'corner_normals') else None

pos, nrm, uv, tris_sword, tris_sheath = [], [], [], [], []
key = {}
for lt in me.loop_triangles:
    ids = []
    for li in lt.loops:
        vi = me.loops[li].vertex_index
        n = np.array(corner_normals[li] if corner_normals else me.loops[li].normal) @ N3.T
        n = n / (np.linalg.norm(n) + 1e-12)
        u = tuple(np.round(uvl[li].uv, 6))
        k = (vi, u, tuple(np.round(n, 3)))
        if k not in key:
            key[k] = len(pos); pos.append(co[vi]); nrm.append(n); uv.append(u)
        ids.append(key[k])
    cx = co[[me.loops[li].vertex_index for li in lt.loops]][:, 0].mean()
    (tris_sword if cx < 0 else tris_sheath).append(ids)
pos, nrm, uv = np.array(pos), np.array(nrm), np.array(uv)

def remap(P):
    # source (x width, y thickness, z length) -> limb (X length, Y width, Z thickness); a cyclic swap, no mirror
    return np.stack([P[:, 2], P[:, 0], P[:, 1]], 1)

def part(tris, place):
    T = np.array(tris)
    used = np.unique(T)
    idx = -np.ones(len(pos), int); idx[used] = np.arange(len(used))
    P = pos[used].copy(); N = remap(nrm[used])
    axis = np.median(P[:, :2], 0)  # centre line of the part in source x/y
    P[:, 0] -= axis[0]; P[:, 1] -= axis[1]
    return place(remap(P) * UNITS), N, uv[used], idx[T], P

def sword_place(dx):
    def f(P):
        P[:, 0] += dx - GRIP_TOP * UNITS
        return P
    return f

def write(path, P, N, UV, T, texture):
    with open(path, 'wb') as f:
        f.write(b'LMDL'); f.write(struct.pack('<I', 2))
        f.write(struct.pack('<IIII', len(P), len(T), 21, 0)); f.write(struct.pack('<f', 1.0))
        for b in range(21):
            nm = b'Root' if b == 0 else b'Limb%d' % b
            f.write(struct.pack('<I', len(nm))); f.write(nm); f.write(struct.pack('<i', b - 1 if b else -1))
            f.write(struct.pack('<16f', *np.eye(4).ravel())); f.write(struct.pack('<3f', 0, 0, 0))
        q = np.zeros(len(P), dtype=[('p', '<f4', 3), ('n', '<i1', 3), ('pad', '<u1'), ('uv', '<f4', 2), ('bi', '<u1', 4), ('bw', '<u1', 4)])
        q['p'] = P; q['n'] = np.clip(np.round(N * 127), -127, 127); q['uv'] = UV; q['bw'][:, 0] = 255
        f.write(q.tobytes()); f.write(np.asarray(T).astype('<u4').tobytes())
        f.write(struct.pack('<I', 1)); nm = texture.encode()
        f.write(struct.pack('<I', len(nm))); f.write(nm); f.write(struct.pack('<II', 0, len(T)))
        f.write(struct.pack('<I', 0))
    print('wrote', path, len(P), 'verts', len(T), 'tris')

tex = tex_prefix + '/basecolor'
# Sword in the hand, and the same sword sheathed (its own copy so each is one static list).
P, N, UV, T, _ = part(tris_sword, sword_place(GUARD_HAND_X))
P[:, 1] += HAND_AXIS[0]; P[:, 2] += HAND_AXIS[1]
write(out_dir + '/master-sword.lmdl', P, N, UV, T, tex)
P, N, UV, T, _ = part(tris_sword, sword_place(SHEATHED_GUARD_X))
P[:, 1] += SHEATH_AXIS[0]; P[:, 2] += SHEATH_AXIS[1]
write(out_dir + '/master-sword-sheathed.lmdl', P, N, UV, T, tex)
P, N, UV, T, src_pts = part(tris_sheath, lambda P: P)
P[:, 0] += SHEATH_MOUTH_X - P[:, 0].min(); P[:, 1] += SHEATH_AXIS[0]; P[:, 2] += SHEATH_AXIS[1]
write(out_dir + '/master-sheath.lmdl', P, N, UV, T, tex)
