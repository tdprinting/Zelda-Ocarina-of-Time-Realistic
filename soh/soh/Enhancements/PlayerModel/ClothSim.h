#pragma once

#include "PlayerModelData.h"

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

namespace PlayerModel {

// Verlet chains for the hat tail ("HatCloth0".."HatCloth3") and the tunic skirt
// ("Skirt<k>_0", "Skirt<k>_1", k = 0..7). Each chain is pinned at its first bone, pulled back
// towards the animated pose by a per-joint stiffness, kept at its rest lengths, and pushed out of
// the legs, body and head. The simulated points then re-aim the chain bones.
//
// `pose` holds every bone's transform relative to the actor (the matrix `base` maps it to the
// world). Cloth bones arrive posed rigidly and are replaced with the simulated result.
// `wetness` (0..1) makes the cloth heavier: it sags more and sways less.
void ClothSim_Update(PlayState* play, const Model& model, const Mat4& base, Mat4* pose, float wetness);

// Forget the simulated state so the next update starts from the animated pose.
void ClothSim_Reset();

} // namespace PlayerModel
