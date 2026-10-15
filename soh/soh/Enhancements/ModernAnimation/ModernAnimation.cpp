// Modern animation blending. See ModernAnimation.h and docs/modern-animation/DESIGN.md.

#include "ModernAnimation.h"

#include <cmath>
#include <algorithm>
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
}

namespace {

constexpr float kBinToRad = (float)M_PI / 32768.0f;
constexpr float kRadToBin = 32768.0f / (float)M_PI;
constexpr float kRadToDeg = 180.0f / (float)M_PI;
// A limb whose angular velocity changes by more than this between two frames reads as a pop
constexpr float kPopThresholdDeg = 20.0f;

struct Quat {
    float w, x, y, z;
};

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

// Converts back to the game's Euler angles. Every rotation has two ZYX decompositions; the one closest to
// `near` is returned so the angles stay continuous from frame to frame, which matters because the 60 fps
// frame interpolation blends these angles component by component.
Vec3s QuatToEuler(const Quat& q, const Vec3s& near) {
    float sinPitch = 2.0f * (q.w * q.y - q.z * q.x);
    if (fabsf(sinPitch) > 0.9995f) {
        // Gimbal lock: roll and yaw are no longer separable; keep the reference split.
        return near;
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
    if (len > 0.0f) {
        r = { r.w / len, r.x / len, r.y / len, r.z / len };
    }
    return r;
}

// The original per-component interpolation (SkelAnime_InterpFrameTable), used for the root translation
// and as the reference for picking Euler solutions.
Vec3s LerpComponents(const Vec3s& start, const Vec3s& target, float weight) {
    Vec3s r;
    r.x = (s16)((s16)(target.x - start.x) * weight) + start.x;
    r.y = (s16)((s16)(target.y - start.y) * weight) + start.y;
    r.z = (s16)((s16)(target.z - start.z) * weight) + start.z;
    return r;
}

Vec3s SlerpRotation(const Vec3s& start, const Vec3s& target, float weight) {
    Vec3s reference = LerpComponents(start, target, weight);
    if (start.x == target.x && start.y == target.y && start.z == target.z) {
        return target;
    }
    return QuatToEuler(Slerp(EulerToQuat(start), EulerToQuat(target), weight), reference);
}

void InterpRotationsImpl(s32 limbCount, Vec3s* dst, Vec3s* start, Vec3s* target, f32 weight, bool slerp) {
    for (s32 i = 1; i < limbCount; i++) {
        dst[i] = slerp ? SlerpRotation(start[i], target[i], weight) : LerpComponents(start[i], target[i], weight);
    }
}

f32 Ease(f32 t) {
    t = CLAMP(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// ---- Automatic cross-fades ----

struct BlendState {
    std::vector<Vec3s> from;   // pose on screen when the animation changed
    std::vector<Vec3s> target; // latest unblended pose, for skeletons that stop updating mid-blend
    f32 remaining = 0.0f;      // 1 = all old pose, 0 = blend finished
    f32 rate = 0.0f;
    u32 changeFrame = 0;
    u32 updateFrame = 0;
};

std::unordered_map<SkelAnime*, BlendState> sBlends;
SkelAnime* sPendingChange = nullptr;
u32 sFrame = 1;
Vec3s sScratch[256 + 1];

void ApplyBlend(SkelAnime* skel, BlendState& state) {
    s32 count = MIN((s32)skel->limbCount, (s32)state.from.size());
    f32 fromWeight = ModernAnim_EasingEnabled() ? 1.0f - Ease(1.0f - state.remaining) : state.remaining;
    InterpRotationsImpl(count, skel->jointTable, skel->jointTable, state.from.data(), fromWeight,
                        ModernAnim_QuaternionEnabled());
}

void StoreTable(std::vector<Vec3s>& dst, const SkelAnime* skel) {
    dst.assign(skel->jointTable, skel->jointTable + skel->limbCount);
}

// ---- Head tracking springs ----

struct Spring {
    f32 offset = 0.0f;   // value - target, in binary angle units
    f32 velocity = 0.0f; // per frame
    s16 lastValue = 0;
    u32 frame = 0;
};

std::unordered_map<s16*, Spring> sSprings;

// ---- Test statistics ----

struct LimbHistory {
    u32 changeFrame = 0; // last frame the skeleton switched to a different animation
    std::vector<Quat> prev;
    std::vector<Vec3f> prevVelocity;
    u32 frame = 0;
    s32 samples = 0;
};

bool sStatsEnabled = false;
std::unordered_map<SkelAnime*, LimbHistory> sHistory;
std::vector<SkelAnime*> sUpdatedThisFrame;
ModernAnimFrameStats sStats = {};
std::vector<ModernAnimSkelSample> sSamples;

// Rotation vector (axis * angle, radians) taking a to b
Vec3f DeltaRotation(const Quat& a, const Quat& b) {
    // d = b * conj(a)
    Quat d = { b.w * a.w + b.x * a.x + b.y * a.y + b.z * a.z, -b.w * a.x + b.x * a.w - b.y * a.z + b.z * a.y,
               -b.w * a.y + b.x * a.z + b.y * a.w - b.z * a.x, -b.w * a.z - b.x * a.y + b.y * a.x + b.z * a.w };
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

float SampleStats(SkelAnime* skel, bool isPlayer) {
    LimbHistory& h = sHistory[skel];
    if (h.frame + 1 != sFrame || h.prev.size() != skel->limbCount) {
        h.samples = 0;
    }
    std::vector<Quat> cur(skel->limbCount);
    std::vector<Vec3f> vel(skel->limbCount, { 0.0f, 0.0f, 0.0f });
    float maxAccel = 0.0f;
    s32 worstLimb = 0;
    for (s32 i = 1; i < skel->limbCount; i++) {
        cur[i] = EulerToQuat(skel->jointTable[i]);
        if (h.samples >= 1) {
            vel[i] = DeltaRotation(h.prev[i], cur[i]);
        }
        if (h.samples >= 2) {
            Vec3f d = { vel[i].x - h.prevVelocity[i].x, vel[i].y - h.prevVelocity[i].y,
                        vel[i].z - h.prevVelocity[i].z };
            float accel = sqrtf(d.x * d.x + d.y * d.y + d.z * d.z) * kRadToDeg;
            if (accel > maxAccel) {
                maxAccel = accel;
                worstLimb = i;
            }
        }
    }
    h.prev = std::move(cur);
    h.prevVelocity = std::move(vel);
    h.frame = sFrame;
    h.samples++;

    sStats.skeletonCount++;
    if (isPlayer) {
        sStats.playerMaxAccel = fmaxf(sStats.playerMaxAccel, maxAccel);
        sStats.playerPop |= maxAccel > kPopThresholdDeg;
    } else {
        if (maxAccel > sStats.actorMaxAccel) {
            sStats.actorMaxAccel = maxAccel;
            sStats.actorWorst = skel;
            sStats.actorWorstLimb = worstLimb;
        }
        sStats.actorPops += maxAccel > kPopThresholdDeg;
    }
    return maxAccel;
}

template <typename Map> void EraseRange(Map& map, uintptr_t begin, uintptr_t end) {
    for (auto it = map.begin(); it != map.end();) {
        uintptr_t key = (uintptr_t)it->first;
        it = (key >= begin && key < end) ? map.erase(it) : std::next(it);
    }
}

void ForgetActor(Actor* actor) {
    uintptr_t begin = (uintptr_t)actor;
    uintptr_t end = begin + ActorDB::Instance->RetrieveEntry(actor->id).entry.instanceSize;
    EraseRange(sBlends, begin, end);
    EraseRange(sHistory, begin, end);
    EraseRange(sSprings, begin, end);
    std::erase_if(sUpdatedThisFrame, [&](SkelAnime* s) { return (uintptr_t)s >= begin && (uintptr_t)s < end; });
    if ((uintptr_t)sPendingChange >= begin && (uintptr_t)sPendingChange < end) {
        sPendingChange = nullptr;
    }
}

void ForgetAll() {
    sBlends.clear();
    sHistory.clear();
    sSprings.clear();
    sUpdatedThisFrame.clear();
    sPendingChange = nullptr;
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

s32 ModernAnim_QuaternionEnabled(void) {
    return Enabled() && CVarGetInteger(CVAR_MODERN_ANIM_QUATERNION, 1);
}

s32 ModernAnim_EasingEnabled(void) {
    return Enabled() && CVarGetInteger(CVAR_MODERN_ANIM_EASING, 1);
}

s32 ModernAnim_SubframeEnabled(void) {
    return Enabled() && CVarGetInteger(CVAR_MODERN_ANIM_SUBFRAME, 1);
}

s32 ModernAnim_HeadTrackingEnabled(void) {
    return Enabled() && CVarGetInteger(CVAR_MODERN_ANIM_HEAD_TRACKING, 1);
}

void ModernAnim_InterpFrameTable(s32 limbCount, Vec3s* dst, Vec3s* start, Vec3s* target, f32 weight) {
    if (limbCount <= 0) {
        return;
    }
    if (weight >= 1.0f) {
        memmove(dst, target, limbCount * sizeof(Vec3s));
        return;
    }
    dst[0] = LerpComponents(start[0], target[0], weight);
    InterpRotationsImpl(limbCount, dst, start, target, weight, true);
}

void ModernAnim_InterpRotations(s32 limbCount, Vec3s* dst, Vec3s* start, Vec3s* target, f32 weight) {
    InterpRotationsImpl(limbCount, dst, start, target, CLAMP(weight, 0.0f, 1.0f), ModernAnim_QuaternionEnabled());
}

f32 ModernAnim_MorphStep(f32 prevRemaining, f32 remaining) {
    f32 e0 = Ease(1.0f - prevRemaining);
    f32 e1 = Ease(1.0f - remaining);
    if (e0 >= 1.0f) {
        return 1.0f;
    }
    return (e1 - e0) / (1.0f - e0);
}

f32 ModernAnim_MorphRemaining(f32 remaining) {
    return 1.0f - Ease(1.0f - remaining);
}

void ModernAnim_BeginChange(SkelAnime* skel, void* newAnimation, void* curAnimation, f32 morphFrames, s32 isLink) {
    sPendingChange = nullptr;
    if (sStatsEnabled && newAnimation != curAnimation) {
        sHistory[skel].changeFrame = sFrame;
    }
    s32 frames = 0;
    if (Enabled()) {
        frames = isLink ? CVarGetInteger(CVAR_MODERN_ANIM_PLAYER_BLEND, MODERN_ANIM_PLAYER_BLEND_DEFAULT)
                        : CVarGetInteger(CVAR_MODERN_ANIM_ACTOR_BLEND, MODERN_ANIM_ACTOR_BLEND_DEFAULT);
    }
    // Only instant changes to a different animation get a cross-fade; the game's own morphs are kept
    // Skeletons outside gameplay (title, file select) are never cross-faded: nothing finishes their fades
    if (gPlayState == nullptr || frames <= 0 || morphFrames != 0.0f || newAnimation == curAnimation ||
        curAnimation == nullptr || skel->jointTable == nullptr || skel->limbCount == 0) {
        sBlends.erase(skel);
        return;
    }
    BlendState& state = sBlends[skel];
    // jointTable holds what is on screen: either last frame's final pose, or this frame's already blended one
    StoreTable(state.from, skel);
    state.rate = 1.0f / MIN(frames, MODERN_ANIM_BLEND_MAX);
    state.remaining = 1.0f - state.rate;
    state.changeFrame = sFrame;
    sPendingChange = skel;
}

void ModernAnim_EndChange(SkelAnime* skel) {
    if (sPendingChange != skel) {
        return;
    }
    sPendingChange = nullptr;
    auto it = sBlends.find(skel);
    if (it == sBlends.end()) {
        return;
    }
    StoreTable(it->second.target, skel);
    ApplyBlend(skel, it->second);
}

void ModernAnim_PostUpdate(SkelAnime* skel) {
    if (sStatsEnabled) {
        sUpdatedThisFrame.push_back(skel);
    }
    if (sBlends.empty()) {
        return;
    }
    auto it = sBlends.find(skel);
    if (it == sBlends.end()) {
        return;
    }
    BlendState& state = it->second;
    if (!Enabled()) {
        sBlends.erase(it);
        return;
    }
    // Advance once per game frame; a change made earlier this frame already set the first step
    if (state.changeFrame != sFrame && state.updateFrame != sFrame) {
        state.remaining -= state.rate;
        if (state.remaining <= 0.0f) {
            sBlends.erase(it);
            return;
        }
    }
    state.updateFrame = sFrame;
    StoreTable(state.target, skel);
    ApplyBlend(skel, state);
}

void ModernAnim_Forget(SkelAnime* skel) {
    sBlends.erase(skel);
    sHistory.erase(skel);
    if (sPendingChange == skel) {
        sPendingChange = nullptr;
    }
}

void ModernAnim_FrameEnd(PlayState* play) {
    // Skeletons that changed animation but are not being updated (posed once and left) still need their
    // cross-fade to finish, from the stored poses since nothing reloads jointTable for them.
    for (auto it = sBlends.begin(); it != sBlends.end();) {
        SkelAnime* skel = it->first;
        BlendState& state = it->second;
        if (state.changeFrame == sFrame || state.updateFrame == sFrame) {
            ++it;
            continue;
        }
        state.remaining -= state.rate;
        s32 count = MIN((s32)skel->limbCount, (s32)state.target.size());
        if (state.remaining <= 0.0f || !Enabled()) {
            memcpy(skel->jointTable + 1, state.target.data() + 1, MAX(count - 1, 0) * sizeof(Vec3s));
            it = sBlends.erase(it);
            continue;
        }
        memcpy(skel->jointTable + 1, state.target.data() + 1, MAX(count - 1, 0) * sizeof(Vec3s));
        ApplyBlend(skel, state);
        ++it;
    }

    if (sStatsEnabled) {
        sStats = {};
        Player* player = play != nullptr ? GET_PLAYER(play) : nullptr;
        sSamples.clear();
        // Link's pose is final only now (his queued blends just ran), and some of his actions pose him without
        // LinkAnimation_Update, so he is always sampled. His upper-body skeleton is copied into the main one.
        if (player != nullptr) {
            std::erase_if(sUpdatedThisFrame,
                          [&](SkelAnime* s) { return s == &player->skelAnime || s == &player->upperSkelAnime; });
            sUpdatedThisFrame.insert(sUpdatedThisFrame.begin(), &player->skelAnime);
        }
        std::sort(sUpdatedThisFrame.begin() + (player != nullptr ? 1 : 0), sUpdatedThisFrame.end());
        sUpdatedThisFrame.erase(std::unique(sUpdatedThisFrame.begin(), sUpdatedThisFrame.end()),
                                sUpdatedThisFrame.end());
        for (SkelAnime* skel : sUpdatedThisFrame) {
            bool isPlayer = player != nullptr && skel == &player->skelAnime;
            f32 accel = SampleStats(skel, isPlayer);
            // A transition shows in the frame of the change and the two after it
            sSamples.push_back({ skel, accel, sFrame - sHistory[skel].changeFrame <= 2 });
        }
        sUpdatedThisFrame.clear();
    }

    if ((sFrame & 63) == 0) {
        for (auto it = sSprings.begin(); it != sSprings.end();) {
            it = (sFrame - it->second.frame > 60) ? sSprings.erase(it) : std::next(it);
        }
        for (auto it = sHistory.begin(); it != sHistory.end();) {
            it = (sFrame - it->second.frame > 60) ? sHistory.erase(it) : std::next(it);
        }
    }
    sFrame++;
}

s32 ModernAnim_NextFrame(SkelAnime* skel, s32 frame) {
    s32 next = frame + 1;
    s32 length = (s32)skel->animLength;
    if (skel->mode >= ANIMMODE_LOOP_PARTIAL) {
        if (next > (s32)skel->endFrame) {
            next = (s32)skel->startFrame;
        }
    } else if (skel->mode >= ANIMMODE_ONCE) {
        if (next >= length) {
            next = length - 1;
        }
    } else if (next >= length) {
        next = 0;
    }
    return MAX(next, 0);
}

void ModernAnim_ApplySubframe(SkelAnime* skel, Vec3s* nextFrame) {
    f32 partial = skel->curFrame - (s32)skel->curFrame;
    if (partial > 0.001f) {
        ModernAnim_InterpRotations(skel->limbCount, skel->jointTable, skel->jointTable, nextFrame, partial);
    }
}

Vec3s* ModernAnim_ScratchTable(void) {
    return sScratch;
}

s16 ModernAnim_SmoothTrackToS(s16* pValue, s16 target, s16 scale, s16 step, s16 minStep) {
    if (!ModernAnim_HeadTrackingEnabled()) {
        return Math_SmoothStepToS(pValue, target, scale, step, minStep);
    }
    s16 diff = target - *pValue;
    Spring& spring = sSprings[pValue];
    if (spring.frame + 1 < sFrame || spring.lastValue != *pValue) {
        // New, stale, or changed by someone else since the last step: start from rest at the current value
        spring.velocity = 0.0f;
    }
    spring.offset = (f32)(s16)(*pValue - target);
    spring.frame = sFrame;

    // Critically damped spring (Game Programming Gems 4, "Critically Damped Ease-In/Ease-Out Smoothing").
    // The original moves 1/scale of the remaining distance per frame; a smoothing time of `scale` frames
    // settles in about the same time but starts and stops gently. Speed stays capped at `step` per frame.
    f32 smoothTime = MAX((f32)scale, 1.0f);
    f32 omega = 2.0f / smoothTime;
    f32 x = omega;
    f32 decay = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    f32 change = CLAMP(spring.offset, -(f32)step * smoothTime, (f32)step * smoothTime);
    f32 temp = spring.velocity + omega * change;
    f32 newOffset = (change + temp) * decay;
    spring.velocity = CLAMP((spring.velocity - omega * temp) * decay, -(f32)step, (f32)step);
    if ((spring.offset > 0.0f) != (newOffset > 0.0f)) {
        newOffset = 0.0f; // never overshoot the target
        spring.velocity = 0.0f;
    }
    // Keep moving by at least the original minimum step so the target is always reached
    f32 moved = spring.offset - newOffset;
    if (fabsf(moved) < (f32)minStep) {
        newOffset = spring.offset > 0.0f ? fmaxf(spring.offset - minStep, 0.0f) : fminf(spring.offset + minStep, 0.0f);
    }
    if (fabsf(newOffset) < 1.0f) {
        newOffset = 0.0f;
        spring.velocity = 0.0f;
    }
    *pValue = (s16)(target + (s16)lrintf(newOffset));
    spring.lastValue = *pValue;
    // Like Math_SmoothStepToS, return the distance that remained before this step
    return diff;
}

void ModernAnim_SetStatsEnabled(s32 enabled) {
    sStatsEnabled = enabled != 0;
}

void ModernAnim_GetFrameStats(ModernAnimFrameStats* out) {
    *out = sStats;
}

s32 ModernAnim_GetFrameSamples(const ModernAnimSkelSample** samples) {
    *samples = sSamples.data();
    return (s32)sSamples.size();
}

} // extern "C"
