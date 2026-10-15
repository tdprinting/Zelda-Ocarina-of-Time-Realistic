import os
import bpy, sys, numpy as np, math
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render_lib import *
from pose import pose_mats, skin
from ootskel import read_anim
a=sys.argv[sys.argv.index('--')+1:]
rig, npz, outp = a[0], a[1], a[2]; shots=a[3:]   # shots: anim:frame:az
bpy.ops.wm.open_mainfile(filepath=rig)
src=bpy.data.objects['Link']; mat=src.data.materials[0]
for ob in list(bpy.data.objects): bpy.data.objects.remove(ob)
D=dict(np.load(npz)); s=5000.0
ysc=1.0
me=bpy.data.meshes.new('posed'); n=len(D['pos'])
me.vertices.add(n); me.loops.add(len(D['tris'])*3); me.polygons.add(len(D['tris']))
me.loops.foreach_set('vertex_index',D['tris'].ravel().astype(np.int32))
me.polygons.foreach_set('loop_start',np.arange(0,len(D['tris'])*3,3,dtype=np.int32))
uvl=me.uv_layers.new(); uvl.data.foreach_set('uv',D['uv'][D['tris'].ravel()].ravel())
me.materials.append(mat)
ob=bpy.data.objects.new('posed',me); bpy.context.scene.collection.objects.link(ob)
co=setup_render((700,900),12)
tiles=[]
for k,sh in enumerate(shots):
    an,fi,az=sh.split(':')[:3]
    if an=='bind': P=D['pos'].copy()
    else:
        A=read_anim(an); f=A[int(fi)%len(A)]
        M=pose_mats(D,f,ysc); P=skin(D,M)
    # actor root at origin, skeleton units -> blender metres
    B=np.stack([P[:,0],-P[:,2],P[:,1]],1)/s
    me.vertices.foreach_set('co',B.ravel()); me.update()
    for p in me.polygons: p.use_smooth=True
    tgt=(0,0,0.45); dist=2.4
    if len(sh.split(':'))>3: tgt=tuple(map(float,sh.split(':')[3].split(','))); dist=float(sh.split(':')[4])
    shoot(co,tgt,dist,float(az),float(sh.split(':')[5]) if len(sh.split(':'))>5 else 8,f'{outp}_{k}.png')
    tiles.append(f'{outp}_{k}.png')
from PIL import Image
ims=[Image.open(t) for t in tiles]
c=Image.new('RGB',(ims[0].width*len(ims),ims[0].height))
for i,im in enumerate(ims): c.paste(im,(i*im.width,0))
c.save(outp+'.png')
