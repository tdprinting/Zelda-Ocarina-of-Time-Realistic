import os
# Fit child Link's skeleton (from local oot.o2r) to a model's joint landmarks (Blender metres, -Y forward, Z up).
import json, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ootskel import read_skel, read_anim, rotm
NAMES=['Root','Waist','LowerControl','RightThigh','RightShin','RightFoot','LeftThigh','LeftShin','LeftFoot',
       'UpperControl','Head','Hat','Collar','LeftShoulder','LeftForearm','LeftHand','RightShoulder','RightForearm',
       'RightHand','Sheath','Torso']
def to_skel(p,s): p=np.asarray(p,float); return s*np.array([p[0],p[2],-p[1]])
def from_skel(q,s): q=np.asarray(q,float)/s; return np.array([q[0],-q[2],q[1]])
def aim(R, d):
    """rotate R minimally so its x axis points along d"""
    a=R[:,0]; b=d/np.linalg.norm(d); v=np.cross(a,b); c=a@b
    if np.linalg.norm(v)<1e-9: return R
    K=np.array([[0,-v[2],v[1]],[v[2],0,-v[0]],[-v[1],v[0],0]])
    Q=np.eye(3)+K+K@K*(1/(1+c))
    return Q@R
def fit(lm, s=5000.0):
    L=read_skel(); a=read_anim('gPlayerAnim_link_normal_wait_free')
    fr=a[0].astype(float); fr[0:3]*=0.64
    n=len(L); parent=[-1]*n
    for i,l in enumerate(L):
        if l['child']!=255: parent[l['child']]=i
        if l['sibling']!=255: parent[l['sibling']]=parent[i]
    T={k:to_skel(v,s) for k,v in lm.items() if isinstance(v,list) and len(v)==3 and not isinstance(v[0],list)}
    pos=[np.array(l['trans'],float) for l in L]
    pos[0]=fr[0:3].copy()
    W=[None]*n
    def local(i): 
        M=np.eye(4); M[:3,:3]=rotm(*fr[3*(i+1):3*(i+1)+3]); M[:3,3]=pos[i]; return M
    order=[]
    def walk(i):
        while i!=255:
            order.append(i)
            if L[i]['child']!=255: walk(L[i]['child'])
            i=L[i]['sibling']
    walk(0)
    # placements: limb -> target for its own origin (solved in parent frame)
    place={0:'pelvis',3:'r_hip',6:'l_hip',13:'l_shoulder',16:'r_shoulder',10:'neck'}
    # aims: limb -> (target name of end point, child limb that sits at that end)
    aims={3:('r_knee',4),4:('r_ankle',5),5:('r_toe',None),6:('l_knee',7),7:('l_ankle',8),8:('l_toe',None),
          13:('l_elbow',14),14:('l_wrist',15),15:('l_fingers',None),16:('r_elbow',17),17:('r_wrist',18),18:('r_fingers',None)}
    for i in order:
        P=W[parent[i]] if parent[i]>=0 else np.eye(4)
        if i in place:
            pos[i]=(np.linalg.inv(P)@np.append(T[place[i]],1))[:3]
        M=P@local(i)
        if i in aims:
            tgt,ch=aims[i]; d=T[tgt]-M[:3,3]
            Rw=aim(M[:3,:3],d)
            Rl=P[:3,:3].T@Rw
            # store local rotation by replacing frame rotation matrix
            M=P.copy(); Ml=np.eye(4); Ml[:3,:3]=Rl; Ml[:3,3]=pos[i]; M=P@Ml
            if ch is not None: pos[ch]=np.array([np.linalg.norm(d),0,0])
        W[i]=M
    out=dict(scale=s,names=NAMES,parent=parent,
             jointPos=[[float(x) for x in p] for p in pos],
             bind=[W[i].tolist() for i in range(n)],
             ootHipHeight=1626.0, ourHipHeight=float(T['l_hip'][1]))
    return out
if __name__=='__main__':
    lm=json.load(open(sys.argv[1])); out=fit(lm)
    json.dump(out,open(sys.argv[2],'w'),indent=1)
    for i,nm in enumerate(out['names']):
        print(i,nm,np.round(np.array(out['bind'][i])[:3,3]).astype(int), np.round(out['jointPos'][i]).astype(int))
