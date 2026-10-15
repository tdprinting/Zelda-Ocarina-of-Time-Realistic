#pragma once

// Modern animation blending for every skeleton in the game (Link, NPCs and enemies), modelled on how
// Twilight Princess and Ocarina of Time 3D move their characters. See docs/modern-animation/DESIGN.md.
//
// Only limb rotations are smoothed: the root translation that drives an actor's movement, animation timing
// and frame counters are left exactly as the original game computes them. Hitboxes attached to limbs (swords,
// claws) follow the smoothed pose, since the game derives them from the drawn limb matrices.

#include "z64math.h"

#define CVAR_MODERN_ANIM_ENABLED "gSettings.ModernAnimation.Enabled"
#define CVAR_MODERN_ANIM_QUATERNION "gSettings.ModernAnimation.QuaternionBlending"
#define CVAR_MODERN_ANIM_EASING "gSettings.ModernAnimation.EasedTransitions"
#define CVAR_MODERN_ANIM_SUBFRAME "gSettings.ModernAnimation.SubframeSampling"
#define CVAR_MODERN_ANIM_PLAYER_BLEND "gSettings.ModernAnimation.PlayerBlendFrames"
#define CVAR_MODERN_ANIM_ACTOR_BLEND "gSettings.ModernAnimation.ActorBlendFrames"
#define CVAR_MODERN_ANIM_HEAD_TRACKING "gSettings.ModernAnimation.SmoothHeadTracking"

#define MODERN_ANIM_PLAYER_BLEND_DEFAULT 2
#define MODERN_ANIM_ACTOR_BLEND_DEFAULT 3
#define MODERN_ANIM_BLEND_MAX 8

struct SkelAnime;
struct PlayState;
struct Actor;

#ifdef __cplusplus
extern "C" {
#endif

// Feature queries (each one is false when the master setting is off)
s32 ModernAnim_QuaternionEnabled(void);
s32 ModernAnim_EasingEnabled(void);
s32 ModernAnim_SubframeEnabled(void);
s32 ModernAnim_HeadTrackingEnabled(void);

// Replacement for SkelAnime_InterpFrameTable: entry 0 (root translation) is interpolated linearly as the
// original does, every limb rotation is interpolated along the shortest arc (quaternion slerp).
void ModernAnim_InterpFrameTable(s32 limbCount, Vec3s* dst, Vec3s* start, Vec3s* target, f32 weight);

// Interpolates limb rotations only (entries 1..limbCount-1), leaving the root translation untouched.
void ModernAnim_InterpRotations(s32 limbCount, Vec3s* dst, Vec3s* start, Vec3s* target, f32 weight);

// Morph weights. The original morphs advance linearly; these return the same quantities on an ease-in-out
// curve so limbs accelerate into and settle out of a transition instead of starting and stopping abruptly.
// remaining* are the SkelAnime morphWeight values (1 at the start of a morph, 0 at the end).
f32 ModernAnim_MorphStep(f32 prevRemaining, f32 remaining);
f32 ModernAnim_MorphRemaining(f32 remaining);

// Automatic cross-fade for animation changes the original game makes instantly (zero morph frames).
// Call BeginChange before the new animation's first frame is loaded into jointTable and EndChange after.
void ModernAnim_BeginChange(struct SkelAnime* skelAnime, void* newAnimation, void* curAnimation, f32 morphFrames,
                            s32 isLink);
void ModernAnim_EndChange(struct SkelAnime* skelAnime);
// Call after a SkelAnime has loaded its frame for this update.
void ModernAnim_PostUpdate(struct SkelAnime* skelAnime);
void ModernAnim_Forget(struct SkelAnime* skelAnime);
// Called once per game frame after every actor and Link's animation queue have updated.
void ModernAnim_FrameEnd(struct PlayState* play);

// Sub-frame sampling: blends limb rotations toward the next keyframe by the fractional part of curFrame.
void ModernAnim_ApplySubframe(struct SkelAnime* skelAnime, Vec3s* nextFrame);
s32 ModernAnim_NextFrame(struct SkelAnime* skelAnime, s32 frame);
Vec3s* ModernAnim_ScratchTable(void);

// Critically damped replacement for Math_SmoothStepToS used for NPC and enemy head/torso tracking. Same
// arguments and return value; the turn eases in and out instead of starting at full speed.
s16 ModernAnim_SmoothTrackToS(s16* pValue, s16 target, s16 scale, s16 step, s16 minStep);

// Test instrumentation (tools/anim-test): per-frame pose discontinuity statistics.
typedef struct {
    f32 playerMaxAccel;           // largest limb angular acceleration on Link this frame, degrees/frame^2
    f32 actorMaxAccel;            // largest limb angular acceleration on any other skeleton
    s32 actorPops;                // skeletons (other than Link) with a limb accelerating more than the pop threshold
    s32 playerPop;                // 1 if Link had such a limb
    s32 skeletonCount;            // skeletons sampled
    struct SkelAnime* actorWorst; // skeleton with actorMaxAccel
    s32 actorWorstLimb;
} ModernAnimFrameStats;
typedef struct {
    struct SkelAnime* skelAnime;
    f32 maxAccel;     // largest limb angular acceleration this frame, degrees/frame^2
    s32 inTransition; // the skeleton changed animation within the last two frames
} ModernAnimSkelSample;
void ModernAnim_SetStatsEnabled(s32 enabled);
// Every skeleton sampled this frame, Link's first
s32 ModernAnim_GetFrameSamples(const ModernAnimSkelSample** samples);
void ModernAnim_GetFrameStats(ModernAnimFrameStats* out);

#ifdef __cplusplus
}
#endif
