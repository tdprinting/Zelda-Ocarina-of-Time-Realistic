import os
# Renders the face-pass model in bind pose with blendshapes and eye rotation applied.
# args: rig.blend face.npz eye_basecolor.png outprefix shot...
#   shot = "morph=w,morph=w|eyeYaw,eyePitch|az|el"  (any part may be empty)
import bpy, sys, numpy as np, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_lib import *
a = sys.argv[sys.argv.index('--') + 1:]
rig, npz, eyetex, outp = a[:4]; shots = a[4:]
bpy.ops.wm.open_mainfile(filepath=rig)
mat = bpy.data.objects['Link'].data.materials[0]
for ob in list(bpy.data.objects): bpy.data.objects.remove(ob)
D = dict(np.load(npz, allow_pickle=True)); s = 5000.0
em = bpy.data.materials.new('eye'); em.use_nodes = True
bs = em.node_tree.nodes['Principled BSDF']; tx = em.node_tree.nodes.new('ShaderNodeTexImage')
tx.image = bpy.data.images.load(eyetex); em.node_tree.links.new(tx.outputs['Color'], bs.inputs['Base Color'])
bs.inputs['Roughness'].default_value = 0.08
me = bpy.data.meshes.new('m'); T = D['tris']; n = len(D['pos'])
me.vertices.add(n); me.loops.add(len(T) * 3); me.polygons.add(len(T))
me.loops.foreach_set('vertex_index', T.ravel().astype(np.int32))
me.polygons.foreach_set('loop_start', np.arange(0, len(T) * 3, 3, dtype=np.int32))
uvl = me.uv_layers.new(); uvl.data.foreach_set('uv', D['uv'][T.ravel()].ravel())
me.materials.append(mat); me.materials.append(em)
mi = np.zeros(len(T), np.int32); st, cnt = D['mat_ranges'][1]; mi[st:st + cnt] = 1
me.polygons.foreach_set('material_index', mi)
ob = bpy.data.objects.new('m', me); bpy.context.scene.collection.objects.link(ob)
co = setup_render((800, 800), 12)
names = [str(x) for x in D['names']]
eyeR, eyeL = names.index('EyeR'), names.index('EyeL')
tiles = []
for k, sh in enumerate(shots):
    parts = (sh.split('|') + ['', '', '', '', ''])[:5]
    P = D['pos'].copy()
    for kv in filter(None, parts[0].split(',')):
        nm, w = kv.split('='); w = float(w)
        P[D['morph_idx_' + nm]] += w * D['morph_d_' + nm]
    if parts[1]:
        yaw, pitch = map(math.radians, map(float, parts[1].split(',')))
        Ry = np.array([[math.cos(yaw), 0, math.sin(yaw)], [0, 1, 0], [-math.sin(yaw), 0, math.cos(yaw)]])
        Rx = np.array([[1, 0, 0], [0, math.cos(pitch), -math.sin(pitch)], [0, math.sin(pitch), math.cos(pitch)]])
        Rm = Ry @ Rx
        for b in (eyeR, eyeL):
            c = D['binds'][b][:3, 3]; m = D['bi'][:, 0] == b
            P[m] = (P[m] - c) @ Rm.T + c
    if parts[4]:
        al = math.radians(float(parts[4]))
        Rl = np.array([[1, 0, 0], [0, math.cos(al), -math.sin(al)], [0, math.sin(al), math.cos(al)]])
        for b in (names.index('LidR'), names.index('LidL')):
            c = D['binds'][b][:3, 3]; m = D['bi'][:, 0] == b
            P[m] = (P[m] - c) @ Rl.T + c
    B = np.stack([P[:, 0], -P[:, 2], P[:, 1]], 1) / s
    me.vertices.foreach_set('co', B.ravel()); me.update()
    for p in me.polygons: p.use_smooth = True
    az = float(parts[2]) if parts[2] else 0; el = float(parts[3]) if parts[3] else 0
    shoot(co, (0, -0.13, 0.79), 0.32, az, el, f'{outp}_{k}.png'); tiles.append(f'{outp}_{k}.png')
from PIL import Image
ims = [Image.open(t) for t in tiles]; cols = min(4, len(ims)); rows = (len(ims) + cols - 1) // cols
c = Image.new('RGB', (400 * cols, 400 * rows))
for i, im in enumerate(ims): c.paste(im.resize((400, 400)), ((i % cols) * 400, (i // cols) * 400))
c.save(outp + '.png')
