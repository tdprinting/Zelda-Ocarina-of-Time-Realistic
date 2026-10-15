# Remove the sword/scabbard/back cloth from a Tripo glb by face classification, keep the largest connected body,
# fill and fair the holes, fix patch UVs. Usage: clean_faces.py -- src.glb samples.npz out.blend
import bpy, bmesh, numpy as np, sys, mathutils, json
import matplotlib.colors as mc
from scipy.sparse import coo_matrix
from scipy.sparse.csgraph import connected_components
a=sys.argv[sys.argv.index('--')+1:]; src,samp,out=a[0],a[1],a[2]
cfg=json.load(open(a[3])) if len(a)>3 else {}
bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=src)
o=[o for o in bpy.data.objects if o.type=='MESH'][0]; o.name='Link'; me=o.data
# bake transform
bpy.context.view_layer.objects.active=o; o.select_set(True)
bpy.ops.object.transform_apply(location=True,rotation=True,scale=True)
bm=bmesh.new(); bm.from_mesh(me)
bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
bm.faces.ensure_lookup_table(); bm.verts.ensure_lookup_table()
nf=len(bm.faces)
S=np.load(samp); pc=np.zeros((nf,3)); cnt=np.bincount(S['poly'],minlength=nf)
for k in range(3): pc[:,k]=np.bincount(S['poly'],weights=S['col'][:,k],minlength=nf)/np.maximum(cnt,1)
cen=np.array([f.calc_center_median()[:] for f in bm.faces])
x,y,z=cen.T
h,s,v=mc.rgb_to_hsv(np.clip(pc,0,1)).T
green=(h>0.17)&(h<0.45)&(s>0.15)
hair=(h>0.09)&(h<0.17)&(s>0.3)&(v>0.35)
def capsule(P0,P1,r):
    P0=np.array(P0);P1=np.array(P1);dv=P1-P0; t=np.clip(((cen-P0)@dv)/(dv@dv),0,1)
    return np.linalg.norm(cen-(P0+t[:,None]*dv),axis=1)<r
C=cfg
mask=np.zeros(nf,bool)
for c in C.get('capsules',[]):
    m=capsule(c['p0'],c['p1'],c['r'])
    if c.get('nongreen',True): m&=~green
    if c.get('ymin') is not None: m&=y>c['ymin']
    mask|=m
for b in C.get('boxes',[]):
    lo,hi=np.array(b['min']),np.array(b['max'])
    m=np.all((cen>lo)&(cen<hi),axis=1)
    if b.get('nongreen',True): m&=~green
    if b.get('nonhair',True): m&=~hair
    if b.get('greenbelow') is not None: m&=(~green)|(y<b['greenbelow'])
    if b.get('vmax') is not None: m&=v<b['vmax']
    mask|=m
# connectivity over faces sharing vertices, excluding masked faces
fv=[(f.index,v.index) for f in bm.faces for v in f.verts]
fv=np.array(fv); keepf=~mask
# face-vertex bipartite graph
F=fv[keepf[fv[:,0]]]
nv=len(bm.verts)
g=coo_matrix((np.ones(len(F)),(F[:,0],nf+F[:,1])),shape=(nf+nv,nf+nv))
k,lab=connected_components(g,directed=False)
fl=lab[:nf]; fl[mask]=-1
cntc=np.bincount(fl[fl>=0]); big=np.argmax(cntc)
remove=fl!=big
print('faces',nf,'masked',mask.sum(),'removed',remove.sum())
for c in np.argsort(-cntc)[1:8]:
    if cntc[c]==0: break
    m=fl==c
    if m.sum()==0: continue
    print('  comp',cntc[c],cen[m].min(0).round(3),cen[m].max(0).round(3))
np.save(out.replace('.blend','_removed.npy'),remove)
gl=bm.faces.layers.int.new('green')
for f in bm.faces: f[gl]=int(green[f.index] and v[f.index]>C.get('patchvmin',0.3))
bmesh.ops.delete(bm, geom=[bm.faces[i] for i in np.nonzero(remove)[0]], context='FACES')
loose=[v for v in bm.verts if not v.link_faces]; bmesh.ops.delete(bm, geom=loose, context='VERTS')
# make boundaries simple loops
for it in range(6):
    bad=set()
    for e in bm.edges:
        if len(e.link_faces)>2: bad.update(e.link_faces)
    for vv in bm.verts:
        nb=sum(1 for e in vv.link_edges if e.is_boundary)
        if nb>2: bad.update(vv.link_faces)
    # faces hanging by a single vertex/edge (spikes)
    for f in bm.faces:
        if sum(1 for e in f.edges if e.is_boundary)>=2: bad.add(f)
    if not bad: break
    bmesh.ops.delete(bm, geom=list(bad), context='FACES')
    bmesh.ops.delete(bm, geom=[vv for vv in bm.verts if not vv.link_faces], context='VERTS')
    print('cleanup',it,len(bad))
# fill holes
for f in bm.faces: f.material_index=0
bnd=[e for e in bm.edges if e.is_boundary]
print('boundary edges',len(bnd))
avg=np.mean([e.calc_length() for e in bnd]) if bnd else 0
res=bmesh.ops.holes_fill(bm, edges=bnd, sides=0)
for f in res['faces']: f.material_index=1
bmesh.ops.triangulate(bm, faces=res['faces'], quad_method='BEAUTY', ngon_method='BEAUTY')
def patch_edges(): return [e for e in bm.edges if len(e.link_faces)==2 and all(f.material_index==1 for f in e.link_faces)]
for it in range(8):
    pe=[e for e in patch_edges() if e.calc_length()>1.5*avg]
    if not pe: break
    bmesh.ops.subdivide_edges(bm, edges=pe, cuts=1)
    pf=[f for f in bm.faces if f.material_index==1]
    bmesh.ops.triangulate(bm, faces=[f for f in pf if len(f.verts)>3], ngon_method='BEAUTY')
    bmesh.ops.beautify_fill(bm, faces=[f for f in bm.faces if f.material_index==1], edges=patch_edges())
pf=[f for f in bm.faces if f.material_index==1]
inner=[v for v in bm.verts if v.link_faces and all(f.material_index==1 for f in v.link_faces)]
for it in range(300):
    new={}
    for vv in inner:
        nb=[e.other_vert(vv).co for e in vv.link_edges]; c=sum(nb,mathutils.Vector())/len(nb); new[vv]=vv.co+(c-vv.co)*0.6
    for vv,c in new.items(): vv.co=c
print('patch faces',len(pf),'inner',len(inner),'boundary left',sum(1 for e in bm.edges if e.is_boundary),'nonmanifold',sum(1 for e in bm.edges if len(e.link_faces)>2))
from mathutils.bvhtree import BVHTree
from mathutils.geometry import barycentric_transform
uvl=bm.loops.layers.uv[0]
bm.verts.index_update(); bm.verts.ensure_lookup_table()
isgreen={}
orig=[f for f in bm.faces if f.material_index==0]
otri=[]
for f in orig: otri.append(f)
VV=[v.co.copy() for v in bm.verts]
bvh=BVHTree.FromPolygons(VV,[[v.index for v in f.verts] for f in orig])
gorig=[f for f in orig if f[gl]]
gbvh=BVHTree.FromPolygons(VV,[[v.index for v in f.verts] for f in gorig])
for f in pf:
    uvs=[]
    for l in f.loops:
        loc,nrm,fi,dist=gbvh.find_nearest(l.vert.co); of=gorig[fi]
        if dist>C.get('greenmax',0.012):
            loc,nrm,fi,dist=bvh.find_nearest(l.vert.co); of=orig[fi]
        t=[vv.co for vv in of.verts][:3]; u=[ll[uvl].uv for ll in of.loops][:3]
        uvs.append(barycentric_transform(loc,t[0],t[1],t[2],u[0].to_3d(),u[1].to_3d(),u[2].to_3d()).to_2d())
    if max((uvs[i]-uvs[j]).length for i in range(3) for j in range(3))>0.02: uvs=[uvs[0]]*3
    for l,uv in zip(f.loops,uvs): l[uvl].uv=uv
patchflag=bm.faces.layers.int.new('patch')
for f in bm.faces: f[patchflag]=f.material_index; f.material_index=0
bm.normal_update(); bm.to_mesh(me); bm.free()
for p in me.polygons: p.use_smooth=True
print('final tris',sum(len(p.vertices)-2 for p in me.polygons))
bpy.ops.wm.save_as_mainfile(filepath=out)
