#pragma once

// Expressive faces for NPCs and enemies: natural blinks, eyes that look at Link or what the character is doing,
// and a mood (lid shape, brow slant, pupil size) chosen from the animation the character is playing and what
// is happening around it, in the spirit of Twilight Princess's daNpcT_c faces. See
// docs/modern-animation/DESIGN.md.
//
// Read-only with respect to the game: it never changes actor state or the game's random generator. The face
// is drawn by the eye shader from the parameters handed to it around each actor's draw.

#include "z64math.h"

#define CVAR_MODERN_ANIM_NPC_FACES "gSettings.ModernAnimation.NpcFaces"

struct Actor;
struct PlayState;
struct SkelAnime;

#ifdef __cplusplus
extern "C" {
#endif

// Face parameters handed to the eye shader for one actor draw (layout shared with the renderer).
typedef struct {
    f32 lookTarget[3]; // world-space point the eyes look at
    f32 lookWeight;    // 0 = eyes centred, 1 = look at lookTarget
    f32 saccade[2];    // extra eye-space offset (x = the character's left, y = up), -1..1 of the eye's half-width
    f32 upperLid;      // 0 open, 1 closed
    f32 lowerLid;      // 0 rest, 1 meets the upper lid
    f32 tilt;          // + upper lids slant down toward the nose (angry, strain), - up toward the nose (worried, sad)
    f32 pupil;         // 1 = normal size
    u32 flags;         // bit 0: active
} NpcFaceParams;

// Called around actor->draw in Actor_Draw.
void NpcFace_BeginActorDraw(struct PlayState* play, struct Actor* actor);
void NpcFace_EndActorDraw(struct PlayState* play, struct Actor* actor);

// Called from ModernAnim_PostUpdate for every skeleton that updated this frame.
void NpcFace_NoteSkeleton(struct SkelAnime* skelAnime);

// Test instrumentation: this frame's face for an actor, or null if it has none.
const NpcFaceParams* NpcFace_Get(struct Actor* actor, s32* mood);
const char* NpcFace_MoodName(s32 mood);

#ifdef __cplusplus
}
#endif
