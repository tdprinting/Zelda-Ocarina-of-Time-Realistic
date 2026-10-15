# Re-orients the foot and hand bind frames so the model's feet and hands sit the way the original limbs do.
#
# fit.py aims each limb's x axis at the next landmark. That suits the arms and legs, whose original geometry
# runs along x, but not the extremities: the original foot runs along x with its sole on the +y side (so x must
# stay level with the ground, not dip towards the toe), and the original hand runs along +y. Aiming x at the toe
# and the fingers left the feet tilted and splayed and the hands turned in game.
#
# usage: orient_limbs.py model.npz skel_fit.json landmarks.json   (rewrites the binds in both files)
import json, sys, numpy as np

RIGHT_FOOT, LEFT_FOOT, LEFT_HAND, RIGHT_HAND = 5, 8, 15, 18
# Direction from the wrist to the knuckles in the original hand limb's space (gLinkChildLeftHandNearDL).
HAND_DIR = np.array([-50.0, 531.0, 55.0])


def to_skel(p, s):
    p = np.asarray(p, float)
    return s * np.array([p[0], p[2], -p[1]])


def foot_rotation(ankle, toe):
    fwd = toe - ankle
    fwd[1] = 0.0
    x = fwd / np.linalg.norm(fwd)
    y = np.array([0.0, -1.0, 0.0])
    return np.stack([x, y, np.cross(x, y)], 1)


def turn_onto(a, b):
    """Smallest rotation taking unit vector a onto unit vector b."""
    v = np.cross(a, b); c = a @ b
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K / (1 + c)


def orient(binds, lm, s):
    binds = np.array(binds, float)
    for limb, side in ((RIGHT_FOOT, 'r'), (LEFT_FOOT, 'l')):
        binds[limb][:3, :3] = foot_rotation(to_skel(lm[f'{side}_ankle'], s), to_skel(lm[f'{side}_toe'], s))
    u = HAND_DIR / np.linalg.norm(HAND_DIR)
    for limb in (LEFT_HAND, RIGHT_HAND):
        # Same pointing as before for the fingers, now carried by the original hand's own finger direction.
        binds[limb][:3, :3] = binds[limb][:3, :3] @ turn_onto(u, np.array([1.0, 0.0, 0.0]))
    return binds


if __name__ == '__main__':
    npz, fitp, lmp = sys.argv[1:4]
    lm = json.load(open(lmp)); F = json.load(open(fitp)); s = F['scale']
    F['bind'] = orient(F['bind'], lm, s).tolist()
    json.dump(F, open(fitp, 'w'))
    D = dict(np.load(npz, allow_pickle=True))
    D['binds'] = orient(D['binds'], lm, s)
    np.savez(npz, **D)
