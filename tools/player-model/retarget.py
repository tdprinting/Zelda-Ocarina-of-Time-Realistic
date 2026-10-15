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

# Joints that take the original offsets (the rest keep the model's own limb lengths, scaled with their parent).
ORIGINAL_JOINTS = ['LeftThigh', 'RightThigh', 'Head', 'LeftShoulder', 'RightShoulder']
# Uniform scale of each part about its joint. The torso matches the original's shoulder-to-hip length
# (996 / 1273). The head shrinks a little less than the body, as on the original the head is large for the body,
# and the boots come down to the original's smaller feet.
SCALE = {'Root': 0.78, 'Waist': 0.78, 'LowerControl': 0.78, 'UpperControl': 0.78, 'Torso': 0.78, 'Collar': 0.78,
         'Sheath': 0.78, 'LeftShoulder': 0.86, 'RightShoulder': 0.86, 'LeftForearm': 0.9, 'RightForearm': 0.9,
         'LeftHand': 0.95, 'RightHand': 0.95, 'LeftThigh': 0.9, 'RightThigh': 0.9,
         'LeftShin': 0.9, 'RightShin': 0.9, 'LeftFoot': 0.86, 'RightFoot': 0.86, 'Head': 0.93}
s = np.ones(nb)
for i, n in enumerate(names):
    if n in SCALE:
        s[i] = SCALE[n]
    elif n.startswith('Skirt'):
        s[i] = SCALE['Waist']
    elif n.startswith('Hat') or n.startswith('Eye') or n.startswith('Lid'):
        s[i] = SCALE['Head']

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
newB = np.zeros_like(B)
M = np.zeros_like(B)  # model space -> refitted model space, per bone
jointPos = [list(map(float, j)) for j in F['jointPos']]
for i in order:
    p = parents[i]
    if p < 0:
        newB[i] = B[i]
    else:
        local = np.linalg.inv(B[p]) @ B[i]  # bind-pose offset and rotation in the parent's frame
        if i < skel_limbs:
            j = vanilla[i] if names[i] in ORIGINAL_JOINTS else local[:3, 3] * s[p]
            jointPos[i] = [float(x) for x in j]
        else:
            j = local[:3, 3] * s[p]  # cloth chains and eyes ride on their parent's scaled shape
        nl = local.copy(); nl[:3, 3] = j
        newB[i] = newB[p] @ nl
    S = np.diag([s[i], s[i], s[i], 1.0])
    M[i] = newB[i] @ S @ np.linalg.inv(B[i])

P, N, BI, BW = D['pos'], D['nrm'], D['bi'], D['bw']
W = np.zeros((len(P), nb))
for k in range(4):
    np.add.at(W, (np.arange(len(P)), BI[:, k].astype(int)), BW[:, k])
W /= np.maximum(W.sum(1, keepdims=True), 1e-9)
A = np.einsum('vb,bij->vij', W, M)  # blended affine per vertex
P2 = np.einsum('vij,vj->vi', A[:, :3, :3], P) + A[:, :3, 3]
N2 = np.einsum('vij,vj->vi', A[:, :3, :3], N)
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
# The game scales the animation's root height by our hip height over the original's, so the feet stay on the
# ground: the hip height is now measured from the refitted hips to the refitted soles.
hip = 0.5 * (newB[idx['LeftThigh']][1, 3] + newB[idx['RightThigh']][1, 3]) - P2[:, 1].min()
print('height', round(P[:, 1].max() - P[:, 1].min()), '->', round(P2[:, 1].max() - P2[:, 1].min()),
      'hip height', round(F['ourHipHeight']), '->', round(hip))
F['ourHipHeight'] = float(hip)
F['jointPos'] = jointPos
F['bind'] = [newB[i].tolist() for i in range(skel_limbs)]
np.savez(out_npz, **D)
json.dump(F, open(out_fit, 'w'))
print('head centre (ClothSim)', np.round((M[idx['Head']] @ np.array([0, 3900, 330, 1.0]))[:3]))
