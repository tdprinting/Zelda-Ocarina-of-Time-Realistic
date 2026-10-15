#pragma once

// High-detail player model for child Link.
//
// The model (player-model/child-link.lmdl in soh.o2r or a mod pack) is skinned to child Link's
// original 21-limb skeleton, so every vanilla animation drives it. Each frame the game's own limb
// matrices are captured while the vanilla skeleton is walked, the vanilla limb display lists are
// suppressed (held items are kept), and the mesh is skinned on the CPU and drawn as ordinary
// display list geometry so it goes through the normal lighting path.
//
// Extra bones layered on top of the skeleton: hat and tunic cloth chains (simulated, see
// ClothSim.h) and the face rig (eyes, lids, blendshapes, see FaceRig.h).

#ifdef __cplusplus
#include <libultraship/libultraship.h>
extern "C" {
#endif

#include "z64.h"

#define CVAR_PLAYER_MODEL_CHILD "gSettings.PlayerModel.Child"
#define CVAR_PLAYER_MODEL_CLOTH "gSettings.PlayerModel.Cloth"
#define CVAR_PLAYER_MODEL_FACE "gSettings.PlayerModel.Face"
#define CVAR_PLAYER_MODEL_SURFACE "gSettings.PlayerModel.WetAndDirty"

// Debug/test hooks (used by the headless render tests):
//   gSettings.PlayerModel.Debug.Anim   string, a player animation resource to pose Link with
//   gSettings.PlayerModel.Debug.Frame  int, frame of that animation
//   gSettings.PlayerModel.Debug.Yaw    int, degrees to turn Link in place (to see his face)
//   gSettings.PlayerModel.Debug.Wet / .Dirt   float 0..1 forces wetness / dirt (-1 = off)
#define CVAR_PLAYER_MODEL_DEBUG(x) "gSettings.PlayerModel.Debug." x

// Returns a joint table posed by the debug animation, or `jointTable` when none is set.
Vec3s* PlayerModel_DebugJointTable(Vec3s* jointTable);

// True when the skeleton being drawn is child Link's and the replacement model is enabled and loaded.
s32 PlayerModel_ShouldReplace(void** skeleton);

// Called around SkelAnime_DrawFlexLod in Player_DrawImpl.
void PlayerModel_Begin(PlayState* play, void* player, s32 eyeIndex, s32 mouthIndex);
void PlayerModel_OverrideLimb(s32 limbIndex, Gfx** dList, Vec3f* pos);
void PlayerModel_PostLimb(s32 limbIndex);
void PlayerModel_End(PlayState* play);

// Asks the renderer to re-upload a material map (roughness/metal, normal, skin mask) after its
// texture resource was rewritten on the CPU. No-op until the renderer provides it.
void PlayerModel_RefreshMaterialMap(const char* path);

#ifdef __cplusplus
}
#endif
