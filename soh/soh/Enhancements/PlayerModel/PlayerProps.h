#pragma once

// High-detail props on adult Link: the Master Sword in his hand and the sword and sheath on his back.
//
// The models (player-model/master-sword.lmdl, master-sword-sheathed.lmdl and master-sheath.lmdl) sit in the
// limb spaces of adult Link's vanilla display lists. While the skeleton is drawn, the hand and sheath lists that
// show the Master Sword or its sheath are swapped for copies without the sword and sheath triangles (the hand,
// gauntlet and shields stay), the limb matrices are recorded, and the props are drawn once the skeleton is done.

#ifdef __cplusplus
#include <libultraship/libultraship.h>
extern "C" {
#endif

#include "z64.h"

#define CVAR_PLAYER_MODEL_MASTER_SWORD "gSettings.PlayerModel.MasterSword"

// True when the new Master Sword is enabled and its models loaded.
s32 PlayerProps_Enabled(void);

// Called around SkelAnime_DrawFlexLod in Player_DrawImpl.
void PlayerProps_Begin(void);
// After the vanilla limb callback has picked the limb's list; may replace *dList.
void PlayerProps_OverrideLimb(s32 limbIndex, Gfx** dList);
// After the limb has been drawn, with its matrix current.
void PlayerProps_PostLimb(s32 limbIndex);
void PlayerProps_End(PlayState* play);

#ifdef __cplusplus
}
#endif
