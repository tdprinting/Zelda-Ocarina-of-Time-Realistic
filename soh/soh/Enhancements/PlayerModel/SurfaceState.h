#pragma once

// Wet and dirty clothes for the player model.
//
// player-model/child-link.surf maps the base color's UV space (256x256) to a surface class
// (cloth, linen, skin, hair, leather, metal) and the bind pose height of each texel. Wetness and
// dirt are tracked per texel: going into water soaks everything below the water line and washes
// mud off, each material dries at its own rate, and rolling or being knocked down on dirt, sand
// or grass grinds in patchy dirt (mud when wet). The base color and roughness textures are
// rewritten from pristine copies whenever the state has changed enough, a few times a second.

#include <libultraship/libultraship.h>

extern "C" {
#include "z64.h"
}

namespace PlayerModel {

// Advance wetness and dirt by one game frame. `modelScale` converts model units to world units.
void SurfaceState_Update(PlayState* play, Player* player, float modelScale);

// Rewrite the textures if needed; emits the texture cache invalidation into `gfx`.
void SurfaceState_Apply(Gfx** gfx);

// Average wetness (0..1) of the hat and tunic, for the cloth simulation.
float SurfaceState_ClothWetness();

void SurfaceState_Reset();

} // namespace PlayerModel
