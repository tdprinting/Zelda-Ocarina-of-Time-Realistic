#pragma once

// Local split-screen co-op: a second, fully simulated Link driven by controller port 2.
//
// Player 2 is a real Player actor that lives at the tail of the ACTORCAT_PLAYER list, so GET_PLAYER()
// keeps returning Player 1 for the rest of the game. While Player 2 updates or draws, the module swaps
// "the player" context (list head, controller port, main camera) so the stock z_player.c code runs
// unmodified for Player 2. See docs/split-screen-coop.md for the design.

#include "z64.h"

#ifdef __cplusplus
extern "C" {
#endif

// True when the co-op setting is on, whether or not Player 2 is currently spawned.
int Coop_IsEnabled(void);

// Player 2, or NULL when co-op is off or Player 2 is not in the current scene.
Player* Coop_GetPlayer2(void);

// Player 2's camera, or NULL when there is none. Only meaningful while Coop_GetPlayer2() is non-NULL.
Camera* Coop_GetPlayer2Camera(void);

// True when this frame should be drawn as two views: co-op is on, Player 2 is present and neither player is
// in a cutscene or scene transition.
int Coop_IsSplitScreenActive(PlayState* play);

// The screen area for view 0 (Player 1) or view 1 (Player 2), in the game's 320x240 coordinates.
void Coop_GetSplitViewport(int viewIndex, Viewport* viewport);

// Points the view at Player 2's camera (eye, look-at, up, field of view) as of the last camera update.
void Coop_ApplyPlayer2Camera(View* view);

// Draws the HUD (and any open pause menu) once in each player's half. Used instead of Interface_Draw on
// split-screen frames.
void Coop_DrawHuds(PlayState* play);

// Each player's own pause menu. Coop_UpdatePauseSetup runs where Play_Update opens the pause menu: it returns
// -1 when the single-player pause menu is in charge this frame, otherwise whether the game is paused (both
// players' menus open). Coop_UpdatePauseMenus runs where Play_Update updates the pause menu.
int Coop_UpdatePauseSetup(PlayState* play);
void Coop_UpdatePauseMenus(PlayState* play);

#ifdef __cplusplus
}
#endif
