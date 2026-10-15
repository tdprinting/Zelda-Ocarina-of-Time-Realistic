#pragma once

#include "PlayerModelData.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

namespace PlayerModel {

// Drives the face: eye bones (look direction, saccades), eyelid and mouth blendshapes, and the
// expression layer that picks Link's mood from what is happening in the game.
//
// `player` is null when Link is drawn outside gameplay (pause menu); the face then holds the
// expression the animation asks for. `eyeIndex`/`mouthIndex` are the vanilla face texture
// indices the current animation requested. Writes one weight per model morph to `morphWeights`
// and may re-aim face bones in `pose` (actor-relative transforms).
void FaceRig_Update(PlayState* play, Player* player, const Model& model, s32 eyeIndex, s32 mouthIndex,
                    const Mat4& base, Mat4* pose, float* morphWeights);

} // namespace PlayerModel
