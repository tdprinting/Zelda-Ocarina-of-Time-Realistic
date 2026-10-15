#pragma once

// Modern animation smoothing for every skeleton in the game (Link, NPCs and enemies), modelled on how
// Twilight Princess and Ocarina of Time 3D move their characters. See docs/modern-animation/DESIGN.md.
//
// Only what is drawn changes. Joint tables, animation state and everything the game reads back from limb
// matrices (hitboxes, focus and effect positions) keep the original pose: the smoothed limb rotations are
// applied on a separate display matrix stack (Matrix_BeginDisplayStack in sys_matrix.c) while skeletons draw.

#include "z64math.h"

#define CVAR_MODERN_ANIM_ENABLED "gSettings.ModernAnimation.Enabled"
#define CVAR_MODERN_ANIM_EASING "gSettings.ModernAnimation.EasedTransitions"
#define CVAR_MODERN_ANIM_SUBFRAME "gSettings.ModernAnimation.SubframeSampling"
#define CVAR_MODERN_ANIM_PLAYER_BLEND "gSettings.ModernAnimation.PlayerBlendFrames"
#define CVAR_MODERN_ANIM_ACTOR_BLEND "gSettings.ModernAnimation.ActorBlendFrames"

#define MODERN_ANIM_PLAYER_BLEND_DEFAULT 2
#define MODERN_ANIM_ACTOR_BLEND_DEFAULT 3
#define MODERN_ANIM_BLEND_MAX 8

struct SkelAnime;
struct PlayState;

#ifdef __cplusplus
extern "C" {
#endif

// Called by Animation_Change/LinkAnimation_Change before the new animation is loaded. Instant changes to a
// different animation get a cross-fade of the drawn pose.
void ModernAnim_OnChange(struct SkelAnime* skelAnime, void* newAnimation, void* curAnimation, f32 morphFrames,
                         s32 isLink);
// Called after a skeleton's animation update; samples between keyframes for animations below full speed.
void ModernAnim_PostUpdate(struct SkelAnime* skelAnime, s32 isLink);
void ModernAnim_Forget(struct SkelAnime* skelAnime);
// Called once per game frame after every actor and Link's animation queue have updated: works out the pose
// each smoothed skeleton is drawn with this frame.
void ModernAnim_FrameEnd(struct PlayState* play);

// Skeleton drawing (z_skelanime.c). BeginDraw returns whether this joint table has a smoothed pose; pass the
// result to EndDraw. LimbMatrix replaces Matrix_TranslateRotateZYX for a limb whose rotation came from
// jointTable[jointIndex].
s32 ModernAnim_BeginDraw(Vec3s* jointTable);
void ModernAnim_EndDraw(s32 began);
void ModernAnim_LimbMatrix(s32 jointIndex, Vec3f* pos, Vec3s* rot);

// sys_matrix.c: the display matrix stack used while a smoothed skeleton draws
void Matrix_BeginDisplayStack(void);
void Matrix_EndDisplayStack(void);
void Matrix_TranslateRotateZYXDisplay(Vec3f* translation, Vec3s* rotation, Vec3s* displayRot);

// Test instrumentation (tools/anim-test): per-frame statistics on the drawn pose.
typedef struct {
    struct SkelAnime* skelAnime;
    f32 maxAccel;     // largest limb angular acceleration of the drawn pose this frame, degrees/frame^2
    s32 inTransition; // the skeleton changed animation within the last two frames
} ModernAnimSkelSample;
void ModernAnim_SetStatsEnabled(s32 enabled);
// Every skeleton sampled this frame, Link's first
s32 ModernAnim_GetFrameSamples(const ModernAnimSkelSample** samples);

#ifdef __cplusplus
}
#endif
