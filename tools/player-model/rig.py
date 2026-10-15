import bpy, json, sys, numpy as np, mathutils, math
argv=sys.argv[sys.argv.index('--')+1:]
src, fitp, out, target = argv[0], argv[1], argv[2], int(argv[3])
bpy.ops.wm.open_mainfile(filepath=src)
o=bpy.data.objects['Link']
LM=json.load(open(argv[4]))
ntri=sum(len(p.vertices)-2 for p in o.data.polygons)
if ntri>target*1.05:
    m=o.modifiers.new('dec','DECIMATE'); m.decimate_type='COLLAPSE'; m.ratio=target/ntri; m.use_collapse_triangulate=True
    bpy.context.view_layer.objects.active=o; bpy.ops.object.modifier_apply(modifier='dec')
print('tris',sum(len(p.vertices)-2 for p in o.data.polygons))
F=json.load(open(fitp)); s=F['scale']
def b(q): q=np.asarray(q)/s; return mathutils.Vector((q[0],-q[2],q[1]))
B=[np.array(m) for m in F['bind']]; names=F['names']; par=F['parent']
arm=bpy.data.armatures.new('LinkSkel'); ao=bpy.data.objects.new('LinkSkel',arm); bpy.context.scene.collection.objects.link(ao)
bpy.context.view_layer.objects.active=ao; bpy.ops.object.mode_set(mode='EDIT')
ends={3:4,4:5,6:7,7:8,13:14,14:15,16:17,17:18,1:None,20:10,10:None}
deform={1,3,4,5,6,7,8,10,11,13,14,15,16,17,18,20}
eb={}
for i,nm in enumerate(names):
    e=arm.edit_bones.new(nm); h=b(B[i][:3,3]); e.head=h
    if i in ends and ends[i] is not None: e.tail=b(B[ends[i]][:3,3])
    else:
        ln={5:0.06,8:0.06,15:0.06,18:0.06,10:0.25,11:0.08,1:0.08}.get(i,0.03)
        dx=mathutils.Vector(b(B[i][:3,3]+B[i][:3,0]*ln*s))-h
        e.tail=h+dx
    if i==10: e.tail=h+mathutils.Vector((0,0,0.28))
    if i==1: e.tail=h+mathutils.Vector((0,0,-0.09))
    e.use_deform = i in deform
    eb[i]=e
for i,p in enumerate(par):
    if p>=0: eb[i].parent=eb[p]
# cloth chains
def chain(prefix, parent, pts):
    prev=eb[parent]; names=[]
    for k in range(len(pts)-1):
        e=arm.edit_bones.new(f'{prefix}{k}'); e.head=pts[k]; e.tail=pts[k+1]; e.parent=prev; e.use_connect=(k>0); prev=e; names.append(e.name)
    return names
V=mathutils.Vector
hat=chain('HatCloth',11,[V(p) for p in LM['hat_chain']])
skirt=[]
SK=LM['skirt']; c0=V((SK['center'][0],SK['center'][1],0))
for k in range(SK['count']):
    a=2*math.pi*k/SK['count']; d=V((math.sin(a),-math.cos(a),0))
    skirt+=chain(f'Skirt{k}_',1,[c0+d*SK[n+'_r']+V((0,0,SK[n+'_z'])) for n in ('top','mid','bot')])
bpy.ops.object.mode_set(mode='OBJECT')
# heat weights
# heat weights on a watertight voxel proxy, then transfer to the real mesh
px=o.copy(); px.data=o.data.copy(); px.name='Proxy'; bpy.context.scene.collection.objects.link(px)
px.data.remesh_voxel_size=0.005; bpy.context.view_layer.objects.active=px
for ob in bpy.context.view_layer.objects: ob.select_set(False)
px.select_set(True); bpy.ops.object.voxel_remesh()
print('proxy verts',len(px.data.vertices))
for ob in bpy.context.view_layer.objects: ob.select_set(False)
px.select_set(True); ao.select_set(True); bpy.context.view_layer.objects.active=ao
bpy.ops.object.parent_set(type='ARMATURE_AUTO')
for ob in bpy.context.view_layer.objects: ob.select_set(False)
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform
pm=px.data; pm.calc_loop_triangles()
gn=[g.name for g in px.vertex_groups]
PW=np.zeros((len(pm.vertices),len(gn)),np.float32)
for v in pm.vertices:
    for g in v.groups: PW[v.index,g.group]=g.weight
pv=[v.co.copy() for v in pm.vertices]; pt=[tuple(t.vertices) for t in pm.loop_triangles]
bvh=BVHTree.FromPolygons(pv,pt)
print('PW sum',PW.sum(),PW.shape)
OW=np.zeros((len(o.data.vertices),len(gn)),np.float32)
for v in o.data.vertices:
    loc,nrm,fi,d=bvh.find_nearest(v.co)
    t=pt[fi]; a_,b_,c_=pv[t[0]],pv[t[1]],pv[t[2]]
    bc=barycentric_transform(loc,a_,b_,c_,mathutils.Vector((1,0,0)),mathutils.Vector((0,1,0)),mathutils.Vector((0,0,1)))
    OW[v.index]=PW[t[0]]*bc[0]+PW[t[1]]*bc[1]+PW[t[2]]*bc[2]
for j,nm in enumerate(gn):
    g=o.vertex_groups.new(name=nm)
    for i in np.nonzero(OW[:,j]>1e-3)[0]: g.add([int(i)],float(OW[i,j]),'REPLACE')
bpy.data.objects.remove(px)
o.parent=ao; am=o.modifiers.new('arm','ARMATURE'); am.object=ao
print('groups',len(o.vertex_groups))
bpy.ops.wm.save_as_mainfile(filepath=out)
