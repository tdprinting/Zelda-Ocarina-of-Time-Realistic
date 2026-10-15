// Modern animation smoothing. See ModernAnimation.h and docs/modern-animation/DESIGN.md.

#include "ModernAnimation.h"
#include "NpcFace.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

#include <libultraship/bridge/consolevariablebridge.h>

#include "soh/ActorDB.h"
#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"

extern "C" {
#include "functions.h"
#include "macros.h"
#include "variables.h"
#include "z64.h"
extern PlayState* gPlayState;
// z_skelanime.c
void SkelAnime_GetFrameData(AnimationHeader* animation, s32 frame, s32 limbCount, Vec3s* frameTable);
s32 LinkAnimation_LoadFrameDirect(LinkAnimationHeader* animation, s32 frame, s32 limbCount, Vec3s* frameTable);
}

namespace {

constexpr float kBinToRad = (float)M_PI / 32768.0f;
constexpr float kRadToBin = 32768.0f / (float)M_PI;
constexpr float kRadToDeg = 180.0f / (float)M_PI;
constexpr s32 kMaxLimbs = 256;

struct Quat {
    float w, x, y, z;
};

constexpr Quat kIdentity = { 1.0f, 0.0f, 0.0f, 0.0f };

bool Enabled() {
    return CVarGetInteger(CVAR_MODERN_ANIM_ENABLED, 1) != 0;
}

// Limbs are drawn with Matrix_TranslateRotateZYX, so a limb's rotation is Rz(z) * Ry(y) * Rx(x).
Quat EulerToQuat(const Vec3s& e) {
    float hx = e.x * kBinToRad * 0.5f;
    float hy = e.y * kBinToRad * 0.5f;
    float hz = e.z * kBinToRad * 0.5f;
    float cx = cosf(hx), sx = sinf(hx);
    float cy = cosf(hy), sy = sinf(hy);
    float cz = cosf(hz), sz = sinf(hz);
    return { cz * cy * cx + sz * sy * sx, cz * cy * sx - sz * sy * cx, cz * sy * cx + sz * cy * sx,
             sz * cy * cx - cz * sy * sx };
}

s16 RadToBin(float r) {
    return (s16)(s32)lrintf(r * kRadToBin);
}

s32 AngleDistance(const Vec3s& a, const Vec3s& b) {
    return ABS((s16)(a.x - b.x)) + ABS((s16)(a.y - b.y)) + ABS((s16)(a.z - b.z));
}

// Converts back to Euler angles. Every rotation has two ZYX decompositions; the one closest to `near` is
// returned so angles stay continuous, which the 60 fps frame interpolation (per-component) relies on.
Vec3s QuatToEuler(const Quat& q, const Vec3s& near) {
    float sinPitch = 2.0f * (q.w * q.y - q.z * q.x);
    if (fabsf(sinPitch) > 0.9995f) {
        return near; // gimbal lock: roll and yaw no longer separable
    }
    Vec3s a;
    a.x = RadToBin(atan2f(2.0f * (q.w * q.x + q.y * q.z), 1.0f - 2.0f * (q.x * q.x + q.y * q.y)));
    a.y = RadToBin(asinf(sinPitch));
    a.z = RadToBin(atan2f(2.0f * (q.w * q.z + q.x * q.y), 1.0f - 2.0f * (q.y * q.y + q.z * q.z)));
    Vec3s b;
    b.x = (s16)(a.x + 0x8000);
    b.y = (s16)(0x8000 - a.y);
    b.z = (s16)(a.z + 0x8000);
    return AngleDistance(a, near) <= AngleDistance(b, near) ? a : b;
}

Quat Mul(const Quat& a, const Quat& b) {
    return { a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
             a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
}

Quat Conj(const Quat& q) {
    return { q.w, -q.x, -q.y, -q.z };
}

Quat Slerp(const Quat& a, Quat b, float t) {
    float dot = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (dot < 0.0f) {
        b = { -b.w, -b.x, -b.y, -b.z };
        dot = -dot;
    }
    float wa, wb;
    if (dot > 0.9995f) {
        wa = 1.0f - t;
        wb = t;
    } else {
        float theta = acosf(dot);
        float s = sinf(theta);
        wa = sinf((1.0f - t) * theta) / s;
        wb = sinf(t * theta) / s;
    }
    Quat r = { wa * a.w + wb * b.w, wa * a.x + wb * b.x, wa * a.y + wb * b.y, wa * a.z + wb * b.z };
    float len = sqrtf(r.w * r.w + r.x * r.x + r.y * r.y + r.z * r.z);
    return len > 0.0f ? Quat{ r.w / len, r.x / len, r.y / len, r.z / len } : kIdentity;
}

f32 Ease(f32 t) {
    t = CLAMP(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

std::vector<Quat> TableToQuats(const Vec3s* table, s32 count) {
    std::vector<Quat> out(count, kIdentity);
    for (s32 i = 1; i < count; i++) {
        out[i] = EulerToQuat(table[i]);
    }
    return out;
}

// ---- Per-skeleton state ----
// Entry 0 of a joint table is the root translation and is never touched; entries 1.. are limb rotations.

struct SkelState {
    Vec3s* jointTable = nullptr;
    s32 limbCount = 0;
    u32 touchFrame = 0;

    // Cross-fade from the pose on screen when the animation changed
    std::vector<Quat> from;
    f32 remaining = 0.0f; // weight of `from`: 1 = all old pose, 0 = finished
    f32 rate = 0.0f;
    u32 changeFrame = 0;

    // Rotation taking this frame's keyframe pose to the pose between it and the next keyframe
    std::vector<Quat> subframe;
    u32 subframeFrame = 0;

    // Result: rotation applied on top of each joint when drawing, and the drawn pose itself
    std::vector<Quat> delta;
    std::vector<Quat> shown;
    u32 deltaFrame = 0;
    bool hasDelta = false;
};

std::unordered_map<SkelAnime*, SkelState> sSkels;
std::unordered_map<Vec3s*, SkelState*> sByTable;
std::vector<const SkelState*> sDrawStack;
u32 sFrame = 1;
bool sAnyUpdate = false;
Vec3s sScratch[kMaxLimbs + 1];
Vec3s sScratchCurrent[kMaxLimbs + 1];

SkelState& StateFor(SkelAnime* skel) {
    SkelState& state = sSkels[skel];
    if (state.jointTable != skel->jointTable || state.limbCount != skel->limbCount) {
        if (state.jointTable != nullptr) {
            sByTable.erase(state.jointTable);
        }
        state = SkelState();
        state.jointTable = skel->jointTable;
        state.limbCount = skel->limbCount;
    }
    // unordered_map never moves its values, so the pointer stays valid until the entry is erased
    sByTable[skel->jointTable] = &state;
    state.touchFrame = sFrame;
    return state;
}

void EraseSkel(std::unordered_map<SkelAnime*, SkelState>::iterator it) {
    auto byTable = sByTable.find(it->second.jointTable);
    if (byTable != sByTable.end() && byTable->second == &it->second) {
        sByTable.erase(byTable);
    }
    sSkels.erase(it);
}

// ---- Test statistics ----

struct LimbHistory {
    u32 changeFrame = 0;
    std::vector<Quat> prev;
    std::vector<Vec3f> prevVelocity;
    u32 frame = 0;
    s32 samples = 0;
};

bool sStatsEnabled = false;
std::unordered_map<SkelAnime*, LimbHistory> sHistory;
std::vector<SkelAnime*> sUpdatedThisFrame;
std::vector<ModernAnimSkelSample> sSamples;

Vec3f DeltaRotation(const Quat& a, const Quat& b) {
    Quat d = Mul(b, Conj(a));
    if (d.w < 0.0f) {
        d = { -d.w, -d.x, -d.y, -d.z };
    }
    float s = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
    if (s < 1e-6f) {
        return { 0.0f, 0.0f, 0.0f };
    }
    float angle = 2.0f * atan2f(s, d.w);
    return { d.x / s * angle, d.y / s * angle, d.z / s * angle };
}

float SampleStats(SkelAnime* skel, const std::vector<Quat>& pose) {
    LimbHistory& h = sHistory[skel];
    if (h.frame + 1 != sFrame || h.prev.size() != pose.size()) {
        h.samples = 0;
    }
    std::vector<Vec3f> vel(pose.size(), { 0.0f, 0.0f, 0.0f });
    float maxAccel = 0.0f;
    for (size_t i = 1; i < pose.size(); i++) {
        if (h.samples >= 1) {
            vel[i] = DeltaRotation(h.prev[i], pose[i]);
        }
        if (h.samples >= 2) {
            Vec3f d = { vel[i].x - h.prevVelocity[i].x, vel[i].y - h.prevVelocity[i].y,
                        vel[i].z - h.prevVelocity[i].z };
            maxAccel = fmaxf(maxAccel, sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) * kRadToDeg);
        }
    }
    h.prev = pose;
    h.prevVelocity = std::move(vel);
    h.frame = sFrame;
    h.samples++;
    return maxAccel;
}

void SampleAll(PlayState* play) {
    sSamples.clear();
    Player* player = play != nullptr ? GET_PLAYER(play) : nullptr;
    // Link's pose is final only now (his queued blends just ran) and some of his actions pose him without
    // LinkAnimation_Update, so he is always sampled. His upper-body skeleton is copied into the main one.
    if (player != nullptr) {
        std::erase_if(sUpdatedThisFrame,
                      [&](SkelAnime* s) { return s == &player->skelAnime || s == &player->upperSkelAnime; });
    }
    std::sort(sUpdatedThisFrame.begin(), sUpdatedThisFrame.end());
    sUpdatedThisFrame.erase(std::unique(sUpdatedThisFrame.begin(), sUpdatedThisFrame.end()), sUpdatedThisFrame.end());
    if (player != nullptr) {
        sUpdatedThisFrame.insert(sUpdatedThisFrame.begin(), &player->skelAnime);
    }
    for (SkelAnime* skel : sUpdatedThisFrame) {
        if (skel->jointTable == nullptr || skel->limbCount == 0) {
            continue;
        }
        auto it = sSkels.find(skel);
        bool drawnSmoothed = it != sSkels.end() && it->second.hasDelta && it->second.deltaFrame == sFrame;
        float accel =
            SampleStats(skel, drawnSmoothed ? it->second.shown : TableToQuats(skel->jointTable, skel->limbCount));
        // A transition shows in the frame of the change and the two after it
        sSamples.push_back({ skel, accel, sFrame - sHistory[skel].changeFrame <= 2 });
    }
    sUpdatedThisFrame.clear();
}

// ---- Lifetime ----

bool InRange(const void* p, uintptr_t begin, uintptr_t end) {
    return (uintptr_t)p >= begin && (uintptr_t)p < end;
}

void ForgetActor(Actor* actor) {
    uintptr_t begin = (uintptr_t)actor;
    uintptr_t end = begin + ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
    for (auto it = sSkels.begin(); it != sSkels.end();) {
        auto next = std::next(it);
        if (InRange(it->first, begin, end)) {
            EraseSkel(it);
        }
        it = next;
    }
    std::erase_if(sHistory, [&](const auto& entry) { return InRange(entry.first, begin, end); });
    std::erase_if(sUpdatedThisFrame, [&](SkelAnime* s) { return InRange(s, begin, end); });
}

void ForgetAll() {
    sSkels.clear();
    sByTable.clear();
    sHistory.clear();
    sUpdatedThisFrame.clear();
}

void RegisterModernAnimation() {
    // ShouldActorDestroy also covers actors destroyed without being deleted (see AlwaysOnFixes.cpp)
    COND_HOOK(ShouldActorDestroy, true, [](void* actor, bool* result) { ForgetActor((Actor*)actor); });
    COND_HOOK(OnActorDestroy, true, [](void* actor) { ForgetActor((Actor*)actor); });
    COND_HOOK(OnPlayDestroy, true, ForgetAll);
}

} // namespace

static RegisterShipInitFunc initFunc(RegisterModernAnimation);

extern "C" {

void ModernAnim_OnChange(SkelAnime* skel, void* newAnimation, void* curAnimation, f32 morphFrames, s32 isLink) {
    if (sStatsEnabled && newAnimation != curAnimation) {
        sHistory[skel].changeFrame = sFrame;
    }
    s32 frames = 0;
    if (Enabled() && gPlayState != nullptr) {
        frames = isLink ? CVarGetInteger(CVAR_MODERN_ANIM_PLAYER_BLEND, MODERN_ANIM_PLAYER_BLEND_DEFAULT)
                        : CVarGetInteger(CVAR_MODERN_ANIM_ACTOR_BLEND, MODERN_ANIM_ACTOR_BLEND_DEFAULT);
        // Link's upper body is copied into his main skeleton; only the main one is cross-faded
        if (isLink && skel == &GET_PLAYER(gPlayState)->upperSkelAnime) {
            frames = 0;
        }
    }
    // Only instant changes to a different animation get a cross-fade; the game's own morphs are left alone
    if (frames <= 0 || morphFrames != 0.0f || newAnimation == curAnimation || curAnimation == nullptr ||
        skel->jointTable == nullptr || skel->limbCount == 0 || skel->limbCount > kMaxLimbs) {
        auto it = sSkels.find(skel);
        if (it != sSkels.end()) {
            it->second.remaining = 0.0f;
        }
        return;
    }
    SkelState& state = StateFor(skel);
    // Fade from what was on screen: the smoothed pose if there was one, else the joint table as it stands
    if (state.hasDelta && state.deltaFrame + 1 >= sFrame && state.shown.size() == (size_t)skel->limbCount) {
        state.from = state.shown;
    } else {
        state.from = TableToQuats(skel->jointTable, skel->limbCount);
    }
    state.rate = 1.0f / MIN(frames, MODERN_ANIM_BLEND_MAX);
    state.remaining = 1.0f - state.rate;
    state.changeFrame = sFrame;
}

void ModernAnim_PostUpdate(SkelAnime* skel, s32 isLink) {
    sAnyUpdate = true;
    if (!isLink) {
        NpcFace_NoteSkeleton(skel);
    }
    if (sStatsEnabled) {
        sUpdatedThisFrame.push_back(skel);
    }
    if (!Enabled() || !CVarGetInteger(CVAR_MODERN_ANIM_SUBFRAME, 1) || gPlayState == nullptr ||
        skel->jointTable == nullptr || skel->animation == nullptr || skel->limbCount == 0 ||
        skel->limbCount > kMaxLimbs) {
        return;
    }
    // Interpolating modes (and the game's own morphs) already move between keyframes
    if (!isLink && (skel->mode == ANIMMODE_LOOP_INTERP || skel->mode == ANIMMODE_ONCE_INTERP ||
                    skel->mode == ANIMMODE_LOOP_PARTIAL_INTERP)) {
        return;
    }
    if (skel->morphWeight != 0.0f) {
        return;
    }
    s32 frame = (s32)skel->curFrame;
    f32 partial = skel->curFrame - frame;
    if (partial < 0.001f) {
        return;
    }
    s32 next = frame + 1;
    s32 length = (s32)skel->animLength;
    if (skel->mode == ANIMMODE_LOOP_PARTIAL || skel->mode == ANIMMODE_LOOP_PARTIAL_INTERP) {
        if (next > (s32)skel->endFrame) {
            next = (s32)skel->startFrame;
        }
    } else if (skel->mode == ANIMMODE_ONCE || skel->mode == ANIMMODE_ONCE_INTERP) {
        next = MIN(next, length - 1);
    } else if (next >= length) {
        next = 0;
    }
    next = MAX(next, 0);
    // Link's joint table is still to be combined with his upper-body animation and other blends from the
    // animation queue, so his offsets come from the keyframes themselves
    const Vec3s* current = skel->jointTable;
    if (isLink) {
        if (!LinkAnimation_LoadFrameDirect((LinkAnimationHeader*)skel->animation, frame, skel->limbCount,
                                           sScratchCurrent) ||
            !LinkAnimation_LoadFrameDirect((LinkAnimationHeader*)skel->animation, next, skel->limbCount, sScratch)) {
            return;
        }
        current = sScratchCurrent;
    } else {
        SkelAnime_GetFrameData((AnimationHeader*)skel->animation, next, skel->limbCount, sScratch);
    }
    SkelState& state = StateFor(skel);
    state.subframe.assign(skel->limbCount, kIdentity);
    for (s32 i = 1; i < skel->limbCount; i++) {
        Quat cur = EulerToQuat(current[i]);
        state.subframe[i] = Mul(Slerp(cur, EulerToQuat(sScratch[i]), partial), Conj(cur));
    }
    state.subframeFrame = sFrame;
}

void ModernAnim_Forget(SkelAnime* skel) {
    auto it = sSkels.find(skel);
    if (it != sSkels.end()) {
        EraseSkel(it);
    }
    sHistory.erase(skel);
}

void ModernAnim_FrameEnd(PlayState* play) {
    // Nothing animated (the game is paused): keep showing the last frame's poses
    if (!sAnyUpdate) {
        return;
    }
    sAnyUpdate = false;

    bool enabled = Enabled();
    bool eased = CVarGetInteger(CVAR_MODERN_ANIM_EASING, 1) != 0;
    for (auto it = sSkels.begin(); it != sSkels.end();) {
        SkelAnime* skel = it->first;
        SkelState& state = it->second;
        if (state.remaining > 0.0f && state.changeFrame != sFrame) {
            state.remaining = MAX(state.remaining - state.rate, 0.0f);
        }
        bool fading = enabled && state.remaining > 0.0f && state.from.size() == (size_t)state.limbCount;
        bool subframe = enabled && state.subframeFrame == sFrame && state.subframe.size() == (size_t)state.limbCount;
        if (!fading && !subframe) {
            state.hasDelta = false;
            if (sFrame - state.touchFrame > 60) {
                auto next = std::next(it);
                EraseSkel(it);
                it = next;
                continue;
            }
            ++it;
            continue;
        }
        // A skeleton's state is dropped when it is freed or re-initialised or its actor is destroyed, so its
        // joint table is still valid here.
        f32 fromWeight = fading ? (eased ? 1.0f - Ease(1.0f - state.remaining) : state.remaining) : 0.0f;
        state.delta.assign(state.limbCount, kIdentity);
        state.shown.assign(state.limbCount, kIdentity);
        for (s32 i = 1; i < state.limbCount; i++) {
            Quat original = EulerToQuat(skel->jointTable[i]);
            Quat pose = subframe ? Mul(state.subframe[i], original) : original;
            if (fading) {
                pose = Slerp(pose, state.from[i], fromWeight);
            }
            state.shown[i] = pose;
            state.delta[i] = Mul(pose, Conj(original));
        }
        state.hasDelta = true;
        state.deltaFrame = sFrame;
        state.touchFrame = sFrame;
        ++it;
    }

    if (sStatsEnabled) {
        SampleAll(play);
    }
    if ((sFrame & 63) == 0) {
        std::erase_if(sHistory, [](const auto& entry) {
            return sFrame - entry.second.frame > 60 && sFrame - entry.second.changeFrame > 60;
        });
    }
    sFrame++;
}

s32 ModernAnim_BeginDraw(Vec3s* jointTable) {
    const SkelState* state = nullptr;
    if (!sByTable.empty() && Enabled()) {
        auto it = sByTable.find(jointTable);
        // The pose worked out at the end of the latest game frame
        if (it != sByTable.end() && it->second->hasDelta && it->second->deltaFrame + 1 >= sFrame) {
            state = it->second;
        }
    }
    sDrawStack.push_back(state);
    if (state == nullptr) {
        return false;
    }
    Matrix_BeginDisplayStack();
    return true;
}

void ModernAnim_EndDraw(s32 began) {
    if (!sDrawStack.empty()) {
        sDrawStack.pop_back();
    }
    if (began) {
        Matrix_EndDisplayStack();
    }
}

void ModernAnim_LimbMatrix(s32 jointIndex, Vec3f* pos, Vec3s* rot) {
    const SkelState* state = sDrawStack.empty() ? nullptr : sDrawStack.back();
    if (state != nullptr && jointIndex > 0 && jointIndex < (s32)state->delta.size()) {
        const Quat& d = state->delta[jointIndex];
        if (fabsf(d.w) < 0.999999f) {
            // `rot` may already carry the actor's own adjustments (head tracking); the smoothing goes on top
            Vec3s displayRot = QuatToEuler(Mul(d, EulerToQuat(*rot)), *rot);
            Matrix_TranslateRotateZYXDisplay(pos, rot, &displayRot);
            return;
        }
    }
    Matrix_TranslateRotateZYX(pos, rot);
}

void ModernAnim_SetStatsEnabled(s32 enabled) {
    sStatsEnabled = enabled != 0;
}

s32 ModernAnim_GetFrameSamples(const ModernAnimSkelSample** samples) {
    *samples = sSamples.data();
    return (s32)sSamples.size();
}

} // extern "C"
