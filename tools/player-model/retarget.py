# Refits the model to child Link's original proportions.
#
# The source model is built like an older child: broad shoulders and hips and a long torso and neck. fit.py placed
# the skeleton on the model, so in game Link stood on the model's proportions and looked too big. Here the model is
# deformed onto the original skeleton instead: the shoulder, hip and neck joints move to the original joint
# offsets, and each body part is scaled about its joint so the mesh fills the new skeleton (a narrower, shorter
# torso; slimmer arms and thighs). The deformation is linear blend skinning with one similarity per bone, so the
# skin weights blend the parts smoothly; morph targets, eye centres and the cloth chains follow their bones.
#
# usage: retarget.py model.npz skel_fit.json out.npz out_skel_fit.json   (OOT_O2R must point at your oot.o2r)
import os, sys, json, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ootskel import read_skel

npz, fitp, out_npz, out_fit = sys.argv[1:5]
D = dict(np.load(npz, allow_pickle=True))
F = json.load(open(fitp))
names = [str(n) for n in D['names']]
B = np.array(D['binds'], float)
parents = [int(p) for p in D['parents']]
nb = len(names)
vanilla = [np.array(l['trans'], float) for l in read_skel()]
idx = {n: i for i, n in enumerate(names)}
GROUND = -20.0  # model units; the original's soles sit about 50 below the actor's origin when standing

# Joints that take the original offsets (the rest keep the model's own limb lengths, scaled with their parent).
ORIGINAL_JOINTS = ['Head']
# Scale of each part about its joint, uniform or (x, y, z) in model space (x across, y up, z forward). The torso is
# shortened to about the original's shoulder-to-hip length (996 / 1273) but keeps most of its width and depth, so Link
# keeps a child's build instead of looking thin; the hips and shoulders follow the body's scale. The head, arms
# and legs keep the model's own size.
SCALE = {'Root': (0.86, 0.77, 0.86), 'Waist': (0.86, 0.77, 0.86), 'LowerControl': (0.86, 0.77, 0.86),
         'UpperControl': (1.0, 0.77, 0.9), 'Torso': (1.0, 0.77, 0.9), 'Collar': (1.0, 0.77, 0.9),
         'Sheath': (1.0, 0.77, 0.9), 'LeftFoot': 0.95, 'RightFoot': 0.95}
s = np.ones((nb, 3))
for i, n in enumerate(names):
    if n in SCALE:
        s[i] = SCALE[n]
    elif n.startswith('Skirt'):
        s[i] = SCALE['Waist']

order = []
def visit(i):
    order.append(i)
    for c in range(nb):
        if parents[c] == i:
            visit(c)
for r in range(nb):
    if parents[r] < 0:
        visit(r)

skel_limbs = len(F['names'])
newB = B.copy()  # bind rotations are kept; only the joint positions move
M = np.zeros_like(B)  # model space -> refitted model space, per bone
jointPos = [list(map(float, j)) for j in F['jointPos']]
for i in order:
    p = parents[i]
    if p >= 0:
        if names[i] in ORIGINAL_JOINTS:
            newB[i, :3, 3] = newB[p, :3, 3] + B[p, :3, :3] @ vanilla[i]
        else:
            newB[i, :3, 3] = newB[p, :3, 3] + s[p] * (B[i, :3, 3] - B[p, :3, 3])
        if i < skel_limbs:
            jointPos[i] = [float(x) for x in B[p, :3, :3].T @ (newB[i, :3, 3] - newB[p, :3, 3])]
    M[i] = np.eye(4)
    M[i, :3, :3] = np.diag(s[i])
    M[i, :3, 3] = newB[i, :3, 3] - s[i] * B[i, :3, 3]

P, N, BI, BW = D['pos'], D['nrm'], D['bi'], D['bw']
W = np.zeros((len(P), nb))
for k in range(4):
    np.add.at(W, (np.arange(len(P)), BI[:, k].astype(int)), BW[:, k])
W /= np.maximum(W.sum(1, keepdims=True), 1e-9)
A = np.einsum('vb,bij->vij', W, M)  # blended affine per vertex
P2 = np.einsum('vij,vj->vi', A[:, :3, :3], P) + A[:, :3, 3]
N2 = np.einsum('vji,vj->vi', np.linalg.inv(A[:, :3, :3]), N)  # inverse transpose: the scale is not uniform
N2 /= np.maximum(np.linalg.norm(N2, axis=1, keepdims=True), 1e-9)
D['pos'], D['nrm'], D['binds'] = P2, N2, newB
for k in list(D.keys()):
    if k.startswith('morph_d_'):
        vi = D['morph_idx_' + k[len('morph_d_'):]].astype(int)
        D[k] = np.einsum('vij,vj->vi', A[vi, :3, :3], D[k])
if 'eye_centres' in D:
    ec = np.array(D['eye_centres'], float)
    for e, bone in enumerate(['EyeR', 'EyeL']):
        ec[e] = (M[idx[bone]] @ np.append(ec[e], 1))[:3]
    D['eye_centres'] = ec
# The game scales the animation's root height by our hip height over the original's. It is calibrated on the
# standing animation so the soles rest where the original's do (its boots sink very slightly into the floor).
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pose import pose_mats, skin
from ootskel import read_anim
D['pos'] = P2
D['jointPos'] = np.array(jointPos)
idle = read_anim('gPlayerAnim_link_normal_wait_free')[0]
feet = np.isin(BI[:, 0], [idx['LeftFoot'], idx['RightFoot']])
ysc = 1.0
for _ in range(4):
    sole = skin(D, pose_mats(D, idle, ysc))[feet, 1].min()
    ysc -= (sole - GROUND) / (idle[1] * 0.64)
del D['jointPos']
print('height', round(P[:, 1].max() - P[:, 1].min()), '->', round(P2[:, 1].max() - P2[:, 1].min()),
      'root height scale', round(F['ourHipHeight'] / F['ootHipHeight'], 3), '->', round(ysc, 3))
F['ourHipHeight'] = float(ysc * F['ootHipHeight'])
F['jointPos'] = jointPos
F['bind'] = [newB[i].tolist() for i in range(skel_limbs)]
np.savez(out_npz, **D)
json.dump(F, open(out_fit, 'w'))
print('head centre (ClothSim)', np.round((M[idx['Head']] @ np.array([0, 3900, 330, 1.0]))[:3]))
print('scaled widths', {n: s[idx[n]].tolist() for n in ('Torso', 'Waist')})
