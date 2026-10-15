import os
# Parse child Link skeleton + player animations from a local oot.o2r (never stored or committed).
import zipfile, struct, numpy as np, math
O2R=os.environ.get('OOT_O2R', 'oot.o2r')  # the game archive, never committed
_z=None
def z():
    global _z
    if _z is None: _z=zipfile.ZipFile(O2R)
    return _z
class R:
    def __init__(s,d): s.d=d; s.p=64
    def u8(s): v=s.d[s.p]; s.p+=1; return v
    def i8(s): v=struct.unpack_from('<b',s.d,s.p)[0]; s.p+=1; return v
    def u16(s): v=struct.unpack_from('<H',s.d,s.p)[0]; s.p+=2; return v
    def i16(s): v=struct.unpack_from('<h',s.d,s.p)[0]; s.p+=2; return v
    def u32(s): v=struct.unpack_from('<I',s.d,s.p)[0]; s.p+=4; return v
    def i32(s): v=struct.unpack_from('<i',s.d,s.p)[0]; s.p+=4; return v
    def f(s): v=struct.unpack_from('<f',s.d,s.p)[0]; s.p+=4; return v
    def str(s): n=s.u32(); v=s.d[s.p:s.p+n].decode(); s.p+=n; return v
def read_limb(path):
    r=R(z().read(path))
    lt=r.i8(); st=r.i8(); r.str(); r.u16(); n=r.u32()
    for i in range(n):
        r.u16(); c=r.i32(); r.p+=c*10; c=r.i32(); r.p+=c*8
    r.str(); r.f();r.f();r.f(); r.u16();r.u16();r.u16()
    child=r.str(); sib=r.str(); dl=r.str(); dl2=r.str()
    t=(r.i16(),r.i16(),r.i16()); ci=r.u8(); si=r.u8()
    return dict(path=path,type=lt,trans=t,child=ci,sibling=si,dl=dl,dl2=dl2)
def read_skel(path='objects/object_link_child/gLinkChildSkel'):
    r=R(z().read(path))
    typ=r.u8(); ltype=r.u8(); lc=r.u32(); dlc=r.u32(); ltype2=r.u8(); n=r.u32()
    limbs=[read_limb(r.str()) for i in range(n)]
    return limbs
def read_anim(name):
    r=R(z().read('objects/gameplay_keep/'+name))
    r.u32(); fc=r.i16(); p=r.str().replace('__OTR__','')
    r2=R(z().read(p)); n=r2.u32(); data=np.array(struct.unpack_from('<%dh'%n,r2.d,r2.p))
    per=n//fc
    return data.reshape(fc,per)
def rotm(rx,ry,rz):
    a=[v*2*math.pi/65536 for v in (rx,ry,rz)]
    cx,sx=math.cos(a[0]),math.sin(a[0]); cy,sy=math.cos(a[1]),math.sin(a[1]); cz,sz=math.cos(a[2]),math.sin(a[2])
    Rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]]); Ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]]); Rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
    return Rz@Ry@Rx
def fk(limbs, frame, trans_override=None):
    """frame: s16 array; returns list of 4x4 world matrices (skeleton space, OoT units) per limb and parent list"""
    n=len(limbs); M=[None]*n; parent=[-1]*n
    for i,l in enumerate(limbs):
        if l['child']!=255: parent[l['child']]=i
        if l['sibling']!=255: parent[l['sibling']]=parent[i]
    def walk(i, P):
        while i!=255:
            l=limbs[i]
            if i==0: pos=frame[0:3]
            else: pos=(trans_override[i] if trans_override is not None else l['trans'])
            rot=frame[3*(i+1):3*(i+1)+3]
            T=np.eye(4); T[:3,:3]=rotm(*rot); T[:3,3]=pos
            M[i]=P@T
            if l['child']!=255: walk(l['child'],M[i])
            i=l['sibling']
    walk(0,np.eye(4))
    return M,parent
