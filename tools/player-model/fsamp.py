# Dense surface samples of a glb (for slicing/segmentation views): sample points on faces with color
import bpy, numpy as np, sys
src=sys.argv[-2]; out=sys.argv[-1]
bpy.ops.wm.read_factory_settings(use_empty=True)
if src.endswith('.blend'): bpy.ops.wm.open_mainfile(filepath=src)
else: bpy.ops.import_scene.gltf(filepath=src)
o=[o for o in bpy.data.objects if o.type=='MESH'][0]; me=o.data; me.calc_loop_triangles()
M=np.array(o.matrix_world)
n=len(me.vertices); co=np.zeros(n*3); me.vertices.foreach_get('co',co); co=co.reshape(-1,3)@M[:3,:3].T+M[:3,3]
nt=len(me.loop_triangles); tv=np.zeros(nt*3,dtype=np.int64); me.loop_triangles.foreach_get('vertices',tv); tv=tv.reshape(-1,3)
tl=np.zeros(nt*3,dtype=np.int64); me.loop_triangles.foreach_get('loops',tl); tl=tl.reshape(-1,3)
tp=np.zeros(nt,dtype=np.int64); me.loop_triangles.foreach_get('polygon_index',tp)
uv=np.zeros(len(me.loops)*2); me.uv_layers[0].data.foreach_get('uv',uv); uv=uv.reshape(-1,2)
img=[i for i in bpy.data.images if 'basecolor' in i.name][0]
W,H=img.size; px=np.array(img.pixels[:],dtype=np.float32).reshape(H,W,4)
a=np.linalg.norm(np.cross(co[tv[:,1]]-co[tv[:,0]],co[tv[:,2]]-co[tv[:,0]]),axis=1)/2
K=int(sys.argv[-3]) if len(sys.argv)>3 and sys.argv[-3].isdigit() else 3000000
cnt=np.random.default_rng(0).poisson(a/a.sum()*K)+1
ti=np.repeat(np.arange(nt),cnt)
r=np.random.default_rng(1).random((len(ti),2)); m=r.sum(1)>1; r[m]=1-r[m]
w=np.stack([1-r[:,0]-r[:,1],r[:,0],r[:,1]],1)
p=(co[tv[ti]]*w[:,:,None]).sum(1); u=(uv[tl[ti]]*w[:,:,None]).sum(1)
c=px[np.clip((u[:,1]%1*H).astype(int),0,H-1),np.clip((u[:,0]%1*W).astype(int),0,W-1),:3]
np.savez(out,co=p,col=c,tri=ti,poly=tp[ti])
print(len(p))
