import os
# Engine-equivalent skinning of an exported .npz for preview (numpy). Mirrors the C++ runtime.
import numpy as np, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ootskel import read_skel, read_anim, rotm
def pose_mats(D, frame, rootYScale=1.0):
    L=read_skel(); jp=D['jointPos']; n=len(L)
    fr=frame.astype(float).copy(); fr[0:3]*=0.64; fr[1]*=rootYScale
    M=[None]*n
    def walk(i,P):
        while i!=255:
            pos=fr[0:3] if i==0 else jp[i]
            T=np.eye(4); T[:3,:3]=rotm(*fr[3*(i+1):3*(i+1)+3]); T[:3,3]=pos
            M[i]=P@T
            if L[i]['child']!=255: walk(L[i]['child'],M[i])
            i=L[i]['sibling']
    walk(0,np.eye(4))
    B=D['binds']; par=D['parents']
    allM=list(M)
    for j in range(n,len(B)):
        p=par[j]; allM.append(allM[p]@np.linalg.inv(B[p])@B[j])
    return allM
def skin(D, allM):
    B=D['binds']; S=np.array([allM[j]@np.linalg.inv(B[j]) for j in range(len(B))])
    P=np.concatenate([D['pos'],np.ones((len(D['pos']),1))],1)
    out=np.zeros((len(P),3))
    for k in range(4):
        Sk=S[D['bi'][:,k]]  # (n,4,4)
        out+=D['bw'][:,k:k+1]*np.einsum('nij,nj->ni',Sk,P)[:,:3]
    return out
