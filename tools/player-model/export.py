# Export a rigged model (rig.blend) into the game's .lmdl format plus a debug .npz.
import bpy, bmesh, json, sys, struct, numpy as np, math
argv=sys.argv[sys.argv.index('--')+1:]
src, fitp, lmp, out = argv[0], argv[1], argv[2], argv[3]
bpy.ops.wm.open_mainfile(filepath=src)
o=bpy.data.objects['Link']; arm=bpy.data.objects['LinkSkel'].data
F=json.load(open(fitp)); LM=json.load(open(lmp)); s=F['scale']
me=o.data
me.calc_loop_triangles()
nv=len(me.vertices)
co=np.zeros(nv*3); me.vertices.foreach_get('co',co); co=co.reshape(-1,3)
bones=[b.name for b in arm.bones]
bidx={n:i for i,n in enumerate(bones)}
# ---- raw heat weights
W=np.zeros((nv,len(bones)),np.float32)
gmap={g.index:bidx.get(g.name,-1) for g in o.vertex_groups}
for v in me.vertices:
    for g in v.groups:
        b=gmap.get(g.group,-1)
        if b>=0: W[v.index,b]=g.weight
# bone segments (blender space)
heads=np.array([b.head_local[:] for b in arm.bones]); tails=np.array([b.tail_local[:] for b in arm.bones])
deform=np.array([b.use_deform for b in arm.bones])
def segdist(P,a,b):
    d=b-a; t=np.clip(((P-a)@d)/max(d@d,1e-12),0,1); return np.linalg.norm(P-(a+t[:,None]*d),axis=1),t
# fill unweighted
empty=W.sum(1)<1e-4
print('unweighted verts',empty.sum())
if empty.any():
    P=co[empty]; D=np.stack([segdist(P,heads[i],tails[i])[0] if deform[i] and not bones[i].startswith(('Hat','Skirt')) else np.full(len(P),9.) for i in range(len(bones))],1)
    nn=np.argsort(D,1)[:,:2]; dd=np.take_along_axis(D,nn,1)+1e-4; w=1/dd**2; w/=w.sum(1,keepdims=True)
    for k in range(2): W[np.nonzero(empty)[0],nn[:,k]]=w[:,k]
# ---- keep arms from picking up skirt/thigh weights (voxel proxy fuses hands to the hips)
lowbody=np.zeros(len(bones),bool)
for i_,b_ in enumerate(bones):
    if b_.startswith('Skirt') or b_ in ('Waist','LeftThigh','RightThigh','LeftShin','RightShin'): lowbody[i_]=True
for side in ('Left','Right'):
    hb_,fb_,sb_=bidx[side+'Hand'],bidx[side+'Forearm'],bidx[side+'Shoulder']
    ht=heads[hb_]+(tails[hb_]-heads[hb_])/np.linalg.norm(tails[hb_]-heads[hb_])*LM.get('hand_len',0.075)
    dh=segdist(co,heads[hb_],ht)[0]; df=segdist(co,heads[fb_],tails[fb_])[0]
    mh=dh<LM.get('hand_radius',0.045)
    W[mh]=0; W[mh,hb_]=1.0
    mf=(df<0.05)&~mh
    Wf=W[mf]; Wf[:,lowbody]=0; zz=Wf.sum(1)<1e-4; Wf[zz,fb_]=1; W[mf]=Wf
    print(side,'hand verts',mh.sum(),'forearm verts',mf.sum())
    tails[hb_]=ht
# ---- hat cloth
chain=np.array(LM['hat_chain']); hb=[bidx[f'HatCloth{k}'] for k in range(len(chain)-1)]
seglen=np.linalg.norm(np.diff(chain,axis=0),axis=1); cum=np.concatenate([[0],np.cumsum(seglen)])
best=np.full(nv,9.); tpar=np.zeros(nv)
for k in range(len(chain)-1):
    d,t=segdist(co,chain[k],chain[k+1]); m=d<best; best[m]=d[m]; tpar[m]=(cum[k]+t[m]*seglen[k])/cum[-1]
hatR=LM.get('hat_radius',0.075)
hatm=(best<hatR)&(co[:,1]>0.05)&(co[:,2]>0.58)
headb=bidx['Hat']
for i in np.nonzero(hatm)[0]:
    tt=tpar[i]*(len(hb)); k=min(int(tt),len(hb)-1); f=tt-k
    w=np.zeros(len(bones))
    # blend: at the root fade from the Hat limb
    root=np.clip(tpar[i]/0.12,0,1)
    w[headb]+=1-root
    if k+1<len(hb) and f>0.5: w[hb[k]]+=root*(1.5-f); w[hb[k+1]]+=root*(f-0.5)
    elif k>0 and f<0.5: w[hb[k]]+=root*(0.5+f); w[hb[k-1]]+=root*(0.5-f)
    else: w[hb[k]]+=root
    W[i]=w
print('hat verts',hatm.sum())
# ---- skirt cloth
SK=LM['skirt']; c=np.array(SK['center']); n=SK['count']
rel=co[:,:2]-c; r=np.linalg.norm(rel,axis=1); ang=np.arctan2(rel[:,0],-rel[:,1])%(2*np.pi)
z=co[:,2]
legs=[bidx['RightThigh'],bidx['LeftThigh']]
nearleg=np.min([segdist(co,heads[b],tails[b])[0] for b in legs],0)
nearhand=np.min([segdist(co,heads[bidx[b]],tails[bidx[b]])[0] for b in ['LeftHand','RightHand','LeftForearm','RightForearm']],0)
skm=(z<SK['top_z'])&(z>SK['bot_z']-0.03)&(r>SK.get('min_r',0.09))&(nearleg>SK.get('leg_clear',0.05))&(nearhand>0.05)
waist=bidx['Waist']
for i in np.nonzero(skm)[0]:
    a=ang[i]/(2*np.pi)*n; k0=int(a)%n; k1=(k0+1)%n; fa=a-int(a)
    h=np.clip((SK['top_z']-z[i])/(SK['top_z']-SK['bot_z']),0,1.2)
    w=np.zeros(len(bones))
    stiff=np.clip(1-h/0.25,0,1)  # top of the skirt sticks to the waist
    w[waist]=stiff
    seg=0 if h<0.5 else 1
    for kk,fk in ((k0,1-fa),(k1,fa)):
        w[bidx[f'Skirt{kk}_{seg}']]+=(1-stiff)*fk
    W[i]=w
print('skirt verts',skm.sum())
# ---- limit to 4 influences
idx=np.argsort(-W,1)[:,:4]; wt=np.take_along_axis(W,idx,1); wt/=np.maximum(wt.sum(1,keepdims=True),1e-8)
# ---- split vertices by uv
uvl=me.uv_layers.active.data
nt=len(me.loop_triangles)
tl=np.zeros(nt*3,dtype=np.int64); me.loop_triangles.foreach_get('loops',tl)
lv=np.zeros(len(me.loops),dtype=np.int64); me.loops.foreach_get('vertex_index',lv)
uv=np.zeros(len(me.loops)*2); uvl.foreach_get('uv',uv); uv=uv.reshape(-1,2)
me.calc_normals_split() if hasattr(me,'calc_normals_split') else None
ln=np.zeros(len(me.loops)*3); me.loops.foreach_get('normal',ln); ln=ln.reshape(-1,3)
key=np.concatenate([lv[:,None].astype(np.float64),np.round(uv*65536)],1)
uk,inv=np.unique(key,axis=0,return_inverse=True); inv=inv.ravel()
nvo=len(uk); vsrc=uk[:,0].astype(np.int64)
vuv=np.zeros((nvo,2)); vuv[inv]=uv
vn=np.zeros((nvo,3)); np.add.at(vn,inv,ln); vn/=np.maximum(np.linalg.norm(vn,axis=1,keepdims=True),1e-8)
tris=inv[tl].reshape(-1,3)
# ---- cut triangles that bridge an arm to the body (Tripo fuses hands to the hips)
dom=idx[vsrc][:,0]
armset={bidx[b] for b in ['LeftHand','RightHand','LeftForearm','RightForearm']}
isarm=np.array([d in armset for d in dom])
side=np.array([bones[d].startswith('Left') for d in dom])
islow=np.array([lowbody[d] for d in dom])
mix=isarm[tris].any(1)&islow[tris].any(1)
mix|=isarm[tris].all(1)&~(side[tris].all(1)|~side[tris].any(1))
# only cut if the triangle is outside the elbow area (real arm/torso seams are at the shoulder, not here)
cent=co[vsrc[tris]].mean(1)
for sd in ('Left','Right'):
    hb_=bidx[sd+'Hand']; dh=segdist(cent,heads[hb_],tails[hb_])[0]
    mix|=(~isarm[tris].any(1))&(dh<LM.get('hand_cut_radius',0.032))
print('bridge tris',mix.sum())
tris=tris[~mix]
# cap the holes with centroid fans
from collections import defaultdict
ec=defaultdict(int)
wt_=vsrc[tris]
rep={}
for t,tw in zip(tris,wt_):
    for k in range(3): rep.setdefault(tw[k],t[k])
    for a_,b_ in ((tw[0],tw[1]),(tw[1],tw[2]),(tw[2],tw[0])): ec[(a_,b_)]+=1
bnd={(a_,b_) for (a_,b_) in ec if (b_,a_) not in ec}
nxt={}
for a_,b_ in bnd: nxt[b_]=a_   # reversed for the cap
seen=set(); caps=[]; newp=[]; newn=[]; newuv=[]; newbi=[]; newbw=[]
Pl=list(range(0))
loops=[]
for st in list(nxt):
    if st in seen: continue
    lp=[st]; seen.add(st); c_=nxt.get(st)
    while c_ is not None and c_!=st and c_ not in seen and len(lp)<400: lp.append(c_); seen.add(c_); c_=nxt.get(c_)
    if c_==st and 3<=len(lp)<=120: loops.append([rep[w] for w in lp])
print('cap loops',len(loops))
def tosk(p): p=np.asarray(p); return s*np.stack([p[...,0],p[...,2],-p[...,1]],-1)
def tosk_n(p): return np.stack([p[...,0],p[...,2],-p[...,1]],-1)
P=tosk(co[vsrc]); N=tosk_n(vn)
BI=idx[vsrc]; BW=wt[vsrc]
extra=[]
for lp in loops:
    c=len(P)+len(extra); vv=np.array(lp)
    extra.append(vv)
    for k in range(len(lp)): tris=np.vstack([tris,[lp[k],lp[(k+1)%len(lp)],c]])
if extra:
    addP=np.array([P[v].mean(0) for v in extra]); addN=np.array([N[v].mean(0) for v in extra])
    addN/=np.maximum(np.linalg.norm(addN,axis=1,keepdims=True),1e-8)
    adduv=np.array([vuv[v[0]] for v in extra]); addbi=np.array([BI[v[0]] for v in extra]); addbw=np.array([BW[v[0]] for v in extra])
    P=np.vstack([P,addP]); N=np.vstack([N,addN]); vuv=np.vstack([vuv,adduv]); BI=np.vstack([BI,addbi]); BW=np.vstack([BW,addbw])
# ---- bones: 21 limbs keep OoT indices; extra bones after
names=F['names']; order=names+[b for b in bones if b not in names]
remap=np.array([order.index(b) for b in bones]); BI=remap[BI]
binds=[np.array(m) for m in F['bind']]
parents=list(F['parent'])
for b in order[len(names):]:
    bb=arm.bones[b]; h=tosk(np.array(bb.head_local)); t=tosk(np.array(bb.tail_local))
    x=(t-h); L=np.linalg.norm(x); x/=L
    up=np.array([0,1,0.]) if abs(x[1])<0.9 else np.array([0,0,1.])
    zz=np.cross(x,up); zz/=np.linalg.norm(zz); yy=np.cross(zz,x)
    M=np.eye(4); M[:3,0]=x; M[:3,1]=yy; M[:3,2]=zz; M[:3,3]=h
    binds.append(M); parents.append(order.index(bb.parent.name))
np.savez(out.replace('.lmdl','.npz'),pos=P,nrm=N,uv=vuv,tris=tris,bi=BI,bw=BW,binds=np.array(binds),parents=np.array(parents),
         names=np.array(order),jointPos=np.array(F['jointPos']))
# ---- binary
def wstr(f,s_): b=s_.encode(); f.write(struct.pack('<I',len(b))); f.write(b)
with open(out,'wb') as f:
    f.write(b'LMDL'); f.write(struct.pack('<I',1))
    f.write(struct.pack('<IIII',len(P),len(tris),len(order),0))
    f.write(struct.pack('<f',F['ourHipHeight']/F['ootHipHeight']))
    for i,b in enumerate(order):
        wstr(f,b); f.write(struct.pack('<i',parents[i]))
        f.write(struct.pack('<16f',*binds[i].T.ravel()))
        jp=F['jointPos'][i] if i<len(names) else [0,0,0]
        f.write(struct.pack('<3f',*jp))
    q=np.zeros(len(P),dtype=[('p','<f4',3),('n','<i1',3),('pad','<u1'),('uv','<f4',2),('bi','<u1',4),('bw','<u1',4)])
    q['p']=P; q['n']=np.clip(np.round(N*127),-127,127); q['uv']=vuv; q['bi']=BI
    bw8=np.round(BW*255).astype(int); bw8[:,0]+=255-bw8.sum(1); q['bw']=bw8
    f.write(q.tobytes())
    f.write(tris.astype('<u4').tobytes())
    # materials: one for now
    f.write(struct.pack('<I',1)); wstr(f,'textures/player-model/child-link/basecolor'); f.write(struct.pack('<II',0,len(tris)))
print('verts',len(P),'tris',len(tris),'bones',len(order))
