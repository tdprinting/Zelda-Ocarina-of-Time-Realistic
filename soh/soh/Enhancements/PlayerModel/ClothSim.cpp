#include "ClothSim.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace PlayerModel {

namespace {

struct V3 {
    float x, y, z;
};
static inline V3 operator+(V3 a, V3 b) {
    return { a.x + b.x, a.y + b.y, a.z + b.z };
}
static inline V3 operator-(V3 a, V3 b) {
    return { a.x - b.x, a.y - b.y, a.z - b.z };
}
static inline V3 operator*(V3 a, float s) {
    return { a.x * s, a.y * s, a.z * s };
}
static inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
static inline V3 Cross(V3 a, V3 b) {
    return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
static inline float Len(V3 a) {
    return std::sqrt(Dot(a, a));
}
static inline V3 Norm(V3 a) {
    float l = Len(a);
    return l > 1e-8f ? a * (1.0f / l) : V3{ 0, 0, 0 };
}

static V3 Origin(const Mat4& m) {
    return { m.m[3][0], m.m[3][1], m.m[3][2] };
}
static V3 Apply(const Mat4& m, V3 p) {
    float in[3] = { p.x, p.y, p.z }, out[3];
    TransformPoint(m, in, out);
    return { out[0], out[1], out[2] };
}

// Rotation matrix (column-major 3x3 inside a Mat4) turning unit vector a onto unit vector b.
static Mat4 RotateOnto(V3 a, V3 b) {
    Mat4 r = Identity();
    V3 axis = Cross(a, b);
    float s = Len(axis);
    float c = std::clamp(Dot(a, b), -1.0f, 1.0f);
    if (s < 1e-6f) {
        return r; // parallel (anti-parallel is prevented by the angle limits)
    }
    axis = axis * (1.0f / s);
    float t = 1.0f - c;
    float x = axis.x, y = axis.y, z = axis.z;
    // row-major R[row][col], stored as m[col][row]
    float R[3][3] = {
        { t * x * x + c, t * x * y - s * z, t * x * z + s * y },
        { t * x * y + s * z, t * y * y + c, t * y * z - s * x },
        { t * x * z - s * y, t * y * z + s * x, t * z * z + c },
    };
    for (int row = 0; row < 3; row++) {
        for (int col = 0; col < 3; col++) {
            r.m[col][row] = R[row][col];
        }
    }
    return r;
}

// Rotates unit vector `from` towards unit vector `to` by at most maxAngle radians.
static V3 LimitAngle(V3 from, V3 to, float maxAngle) {
    float c = std::clamp(Dot(from, to), -1.0f, 1.0f);
    float angle = std::acos(c);
    if (angle <= maxAngle) {
        return to;
    }
    V3 axis = Cross(from, to);
    if (Len(axis) < 1e-6f) {
        // Opposite directions: pick any perpendicular axis.
        axis = Cross(from, std::fabs(from.x) < 0.9f ? V3{ 1, 0, 0 } : V3{ 0, 1, 0 });
    }
    axis = Norm(axis);
    // Rodrigues rotation of `from` about axis by maxAngle.
    float cs = std::cos(maxAngle), sn = std::sin(maxAngle);
    return from * cs + Cross(axis, from) * sn + axis * (Dot(axis, from) * (1.0f - cs));
}

struct Chain {
    std::vector<int> bones;       // bones along the chain, root first
    std::vector<float> restLen;   // model units, one per bone
    std::vector<float> stiffness; // per particle (index 0 is pinned)
    float maxAngle;               // radians away from the animated direction
    float damping;
    float radius; // model units, for collisions
    std::vector<V3> x, prev;      // world positions, bones.size() + 1 particles
};

struct Capsule {
    V3 a, b;
    float r;
};

struct State {
    const Model* model = nullptr;
    std::vector<Chain> chains;
    int headBone = -1;
    V3 headCenterLocal{};
    bool valid = false;
    s32 lastFrame = -1;
    V3 lastRoot{};
};

static State sState;

static void Setup(const Model& model) {
    sState = State();
    sState.model = &model;

    auto addChain = [&](const std::vector<int>& bones, std::vector<float> stiffness, float maxAngle,
                        float damping, float radius) {
        for (int b : bones) {
            if (b < 0) {
                return;
            }
        }
        Chain c;
        c.bones = bones;
        for (size_t i = 0; i < bones.size(); i++) {
            V3 p = Origin(model.bones[bones[i]].bind);
            float len;
            if (i + 1 < bones.size()) {
                len = Len(Origin(model.bones[bones[i + 1]].bind) - p);
            } else {
                len = c.restLen.empty() ? 300.0f : c.restLen.back();
            }
            c.restLen.push_back(len);
        }
        c.stiffness = std::move(stiffness);
        c.stiffness.resize(bones.size() + 1, c.stiffness.empty() ? 0.1f : c.stiffness.back());
        c.maxAngle = maxAngle;
        c.damping = damping;
        c.radius = radius;
        sState.chains.push_back(std::move(c));
    };

    // Hat tail: soft and floppy towards the tip.
    addChain({ model.FindBone("HatCloth0"), model.FindBone("HatCloth1"), model.FindBone("HatCloth2"),
               model.FindBone("HatCloth3") },
             { 1.0f, 0.22f, 0.12f, 0.07f, 0.05f }, 1.3f, 0.93f, 90.0f);

    // Tunic skirt panels: stiffer, they mostly swing with the legs.
    for (int k = 0; k < 8; k++) {
        char n0[16], n1[16];
        snprintf(n0, sizeof(n0), "Skirt%d_0", k);
        snprintf(n1, sizeof(n1), "Skirt%d_1", k);
        addChain({ model.FindBone(n0), model.FindBone(n1) }, { 1.0f, 0.3f, 0.2f }, 0.9f, 0.9f, 40.0f);
    }

    sState.headBone = kLimbHead;
    // Centre of the head in bind pose, expressed in the head limb's space.
    V3 headCenter = { 0.0f, 3900.0f, 330.0f };
    sState.headCenterLocal = Apply(model.bones[kLimbHead].invBind, headCenter);
}

static void Collide(V3& p, const std::vector<Capsule>& caps, float pr) {
    for (const Capsule& c : caps) {
        V3 ab = c.b - c.a;
        float t = std::clamp(Dot(p - c.a, ab) / std::max(Dot(ab, ab), 1e-8f), 0.0f, 1.0f);
        V3 q = c.a + ab * t;
        V3 d = p - q;
        float dist = Len(d);
        float minDist = c.r + pr;
        if (dist < minDist) {
            V3 n = dist > 1e-6f ? d * (1.0f / dist) : V3{ 0, 0, 1 };
            p = q + n * minDist;
        }
    }
}

} // namespace

void ClothSim_Reset() {
    sState.valid = false;
}

void ClothSim_Update(PlayState* play, const Model& model, const Mat4& base, Mat4* pose, float wetness) {
    if (sState.model != &model) {
        Setup(model);
    }
    if (sState.chains.empty()) {
        return;
    }

    const float scale = Len(V3{ base.m[0][0], base.m[0][1], base.m[0][2] });
    if (scale < 1e-8f) {
        return;
    }

    // World transforms of the animated (rigid) pose.
    auto world = [&](int bone) { return Mul(base, pose[bone]); };

    // Collision shapes in world space.
    std::vector<Capsule> caps;
    auto limbCapsule = [&](int a, int b, float r) {
        caps.push_back({ Origin(world(a)), Origin(world(b)), r * scale });
    };
    limbCapsule(kLimbLeftThigh, kLimbLeftShin, 200.0f);
    limbCapsule(kLimbRightThigh, kLimbRightShin, 200.0f);
    limbCapsule(kLimbLeftShin, kLimbLeftFoot, 150.0f);
    limbCapsule(kLimbRightShin, kLimbRightFoot, 150.0f);
    {
        V3 head = Apply(world(sState.headBone), sState.headCenterLocal);
        caps.push_back({ head, head, 620.0f * scale });
        caps.push_back({ Origin(world(kLimbUpper)), head, 430.0f * scale });
    }

    const s32 frame = play != nullptr ? play->gameplayFrames : 0;
    const V3 root = Origin(world(kLimbRoot));
    const bool newFrame = frame != sState.lastFrame;
    if (!sState.valid || (newFrame && frame != sState.lastFrame + 1) ||
        Len(root - sState.lastRoot) > 3000.0f * scale) {
        sState.valid = false;
    }

    const float dt = 1.0f / 20.0f;
    const int substeps = 3;
    const float h = dt / substeps;
    // Gravity of about 400 world units/s^2 for an actor at the usual 0.01 scale.
    // Soaked cloth is heavier: more sag, less swing, and it settles quickly.
    wetness = std::clamp(wetness, 0.0f, 1.0f);
    const V3 gravity = { 0.0f, -40000.0f * scale * (1.0f + 1.5f * wetness), 0.0f };

    for (Chain& c : sState.chains) {
        const size_t n = c.bones.size() + 1;
        std::vector<V3> kin(n);
        for (size_t i = 0; i < c.bones.size(); i++) {
            kin[i] = Origin(world(c.bones[i]));
        }
        {
            Mat4 last = world(c.bones.back());
            kin[n - 1] = Apply(last, V3{ c.restLen.back() * 1.0f, 0.0f, 0.0f });
            // Bone x axis is along the bone; restLen is in model units and Apply includes scale.
        }

        if (!sState.valid || c.x.size() != n) {
            c.x = kin;
            c.prev = kin;
        } else if (newFrame) {
            for (int s = 0; s < substeps; s++) {
                c.x[0] = kin[0];
                c.prev[0] = kin[0];
                for (size_t i = 1; i < n; i++) {
                    V3 v = (c.x[i] - c.prev[i]) * (c.damping - 0.25f * wetness);
                    c.prev[i] = c.x[i];
                    c.x[i] = c.x[i] + v + gravity * (h * h);
                    // Pull towards the animated shape.
                    c.x[i] = c.x[i] + (kin[i] - c.x[i]) * c.stiffness[i];
                }
                for (int iter = 0; iter < 2; iter++) {
                    for (size_t i = 1; i < n; i++) {
                        V3 kinDir = Norm(kin[i] - kin[i - 1]);
                        V3 dir = Norm(c.x[i] - c.x[i - 1]);
                        if (Len(dir) == 0.0f) {
                            dir = kinDir;
                        }
                        dir = LimitAngle(kinDir, dir, c.maxAngle * (1.0f - 0.45f * wetness));
                        c.x[i] = c.x[i - 1] + dir * (c.restLen[i - 1] * scale);
                        Collide(c.x[i], caps, c.radius * scale);
                    }
                }
            }
        }

        // Re-aim each bone from its animated direction to the simulated one.
        for (size_t i = 0; i < c.bones.size(); i++) {
            const int b = c.bones[i];
            V3 kinDir = Norm(kin[i + 1] - kin[i]);
            V3 simDir = Norm(c.x[i + 1] - c.x[i]);
            if (Len(kinDir) == 0.0f || Len(simDir) == 0.0f) {
                continue;
            }
            Mat4 w = world(b);
            Mat4 rot = RotateOnto(kinDir, simDir);
            Mat4 lin = w;
            lin.m[3][0] = lin.m[3][1] = lin.m[3][2] = 0.0f;
            Mat4 nw = Mul(rot, lin);
            nw.m[3][0] = c.x[i].x;
            nw.m[3][1] = c.x[i].y;
            nw.m[3][2] = c.x[i].z;
            pose[b] = Mul(Inverse(base), nw);
        }
    }

    sState.valid = true;
    if (newFrame) {
        sState.lastFrame = frame;
    }
    sState.lastRoot = root;
}

} // namespace PlayerModel
