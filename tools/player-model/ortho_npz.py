import os
# Orthographic front view of an npz model in bind pose, in skeleton units.
# args: rig.blend model.npz out.png cx cy width [az]   (cx,cy = skeleton X,Y at image centre)
import bpy, sys, numpy as np, math, mathutils
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_lib import *
a = sys.argv[sys.argv.index('--') + 1:]
rig, npz, outp = a[0], a[1], a[2]; cx, cy, width = map(float, a[3:6]); az = float(a[6]) if len(a) > 6 else 0
bpy.ops.wm.open_mainfile(filepath=rig)
mat = bpy.data.objects['Link'].data.materials[0]
for ob in list(bpy.data.objects): bpy.data.objects.remove(ob)
D = dict(np.load(npz, allow_pickle=True)); s = 5000.0
me = bpy.data.meshes.new('m'); n = len(D['pos'])
me.vertices.add(n); me.loops.add(len(D['tris']) * 3); me.polygons.add(len(D['tris']))
me.loops.foreach_set('vertex_index', D['tris'].ravel().astype(np.int32))
me.polygons.foreach_set('loop_start', np.arange(0, len(D['tris']) * 3, 3, dtype=np.int32))
uvl = me.uv_layers.new(); uvl.data.foreach_set('uv', D['uv'][D['tris'].ravel()].ravel())
me.materials.append(mat)
P = D['pos']; B = np.stack([P[:, 0], -P[:, 2], P[:, 1]], 1) / s
me.vertices.foreach_set('co', B.ravel()); me.update()
for p in me.polygons: p.use_smooth = True
ob = bpy.data.objects.new('m', me); bpy.context.scene.collection.objects.link(ob)
co = setup_render((800, 800), 8)
co.data.type = 'ORTHO'; co.data.ortho_scale = width / s
t = mathutils.Vector((cx / s, 0, cy / s)); d = mathutils.Vector((math.sin(math.radians(az)), -math.cos(math.radians(az)), 0))
co.location = t + d * 2; co.rotation_euler = (-d).to_track_quat('-Z', 'Y').to_euler()
bpy.context.scene.render.filepath = outp; bpy.ops.render.render(write_still=True)
