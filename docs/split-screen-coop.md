# Split-screen co-op

Local two-player co-op for Ship of Harkinian: a second, fully playable Link on controller port 2, with
its own camera and its own half of the screen.

Turn it on under **Enhancements > Extra Modes > Split-Screen Co-op**. Settings:

| Setting | CVar | Default |
|---|---|---|
| Split-Screen Co-op | `gEnhancements.SplitScreenCoop.Enabled` | off |
| Co-op Screen Layout | `gEnhancements.SplitScreenCoop.Layout` (0 side by side, 1 top and bottom) | side by side |
| Co-op Leash Distance | `gEnhancements.SplitScreenCoop.LeashDistance` (0 = off) | 2000 |

## How it plays

- Player 2 appears beside Player 1 when a scene loads and is driven by whatever device is mapped to
  port 2 in the controller settings (on the AYN Odin 2 Portal: built-in pad on port 1, external pad on
  port 2).
- Both players share one save: inventory, equipment, C-buttons, rupees, magic and hearts. Damage to
  either player comes off the shared heart meter.
- Player 1 leads. Loading zones, doors, void-outs and cutscenes are Player 1's. If Player 2 falls out of
  bounds, walks into a loading zone or strays further than the leash distance, Player 2 is brought back
  behind Player 1 (a fall costs a heart, as it does for Player 1).
- During cutscenes, scene transitions and while Player 1 is dying, Player 2 is hidden and the screen goes
  back to Player 1's full view.
- Enemies go after whichever player is nearer. Either player can talk to NPCs, read signs, collect drops,
  and lift and throw things.
- Each half has its own HUD. Hearts, magic, rupees and the item buttons are shared, so they match; the A
  button shows each player's own action and the minimap arrow shows each player's own position.
- **START opens a small menu in that player's half without pausing the game.** The other player keeps
  playing; only the player with the menu open stops. Controls in the menu:
  - Stick or D-pad: move the cursor.
  - C-left, C-down, C-right (items page): put the item on that C button.
  - A (gear page): equip that sword, shield, tunic or boots.
  - R: switch between the items and gear pages.
  - Z: open the full pause screen (map, quest status, save). This pauses both players as usual.
  - B or START: close.

  Items and gear are shared, so a change from either menu applies to both Links.

## How it works

The code lives in `soh/soh/Enhancements/ExtraModes/SplitScreenCoop.cpp`, plus three small engine
changes.

### Player 2 is a real `Player`

`GET_PLAYER(play)` is the head of the `ACTORCAT_PLAYER` actor list and is used about 900 times across the
game. Player 2 is a `Player` actor (registered in the ActorDB as `Coop_Player`) kept at the **tail** of
that list, so every one of those call sites still sees Player 1.

When Player 2 updates or draws, the module swaps "the player" context for the duration of the call:

- Player 2 is moved to the head of the list, so `GET_PLAYER()` returns Player 2.
- Controller ports 1 and 2 (`play->state.input[0]` and `[1]`) are swapped.
- Player 2's camera trades places with `play->mainCamera` (by contents, because the camera code also
  identifies the main camera by address).

Then the stock `z_player.c` update (`play->playerUpdate`) runs unmodified: sword, shield, items,
climbing, swimming, damage and the camera modes all behave as they do for Player 1. World state that
belongs to Player 1 is snapshotted and restored around Player 2's update (HUD state, Z-target state,
area lighting, respawn points and any scene transition Player 2 tried to start).

### Player 2's camera

Player 2's `Camera` is updated each frame from the `OnCameraState` hook, before the game updates its own
cameras, in the same swapped context. Its eye, look-at, up vector and field of view are kept for the
draw; `play->view` and the environment are put back so Player 1's camera has the final word.

### Drawing two views

`Play_Draw` was split into `Play_ApplyWorldView` (projection, viewing and billboard matrices) and
`Play_DrawWorld` (sky, rooms, actors, effects). With split-screen active, Player 1's view gets the first
half of the screen, then `Play_DrawCoopView` points the view at Player 2's camera, applies the second
half viewport and draws the world again, then restores Player 1's full-screen view for the HUD.

Each view sets its own viewport and scissor, so both halves share one frame and depth buffer. The
interpolation heuristics in `z_view.c` keep one camera history per view (`gViewInterpolationSlot`) so
the two views are not treated as a camera cut every frame.

### Per-player HUD

`Play_DrawOverlayElements` calls `Coop_DrawHuds` instead of `Interface_Draw` on split-screen frames. It
draws the stock HUD twice, once per half. Two small engine additions make the unmodified HUD code land in
a half and keep its layout:

- libultraship gains a `gSPSetHudRegion(x, y, w, h, scalePct)` display-list command. While a region is
  set, 2D rectangles, viewports and scissors are mapped into that part of the screen, and the widescreen
  adjustment uses the region's aspect ratio instead of the window's. A half narrower than 4:3 (side by
  side) instead gets the original 4:3 layout across its width, with every HUD element kept in shape and
  pushed to the nearer of the top and bottom edges, so the hearts and the item buttons don't run into each
  other. `gSPClearHudRegion` turns it off, and every frame starts with it off.
- `OTRGetAspectRatio()` can be overridden (`OTRSetHudAspectRatioOverride`) for the duration of a HUD
  draw, so HUD elements anchored to the screen edges anchor to the edges of the half.

For Player 2's pass, the interface context is pointed at Player 2's own A-button state (the do-action
label, which Player 2's update sets, and its flip animation) and Player 2 is made "the player" for the
minimap arrow.

### Per-player menu

The co-op menu runs from the `OnGameStateMainStart` hook, after the controllers are read and before the
play update. A player's START opens their menu; while it is open, that player's controller input is
consumed by the menu and cleared before gameplay sees it, so their Link stands still while everything else
keeps running. Z hands a START press to Player 1's controller so the stock pause screen opens.

### Other engine changes

- `game.c`: controller 2 no longer drives the debug register editor while co-op is on.

## Known gaps and next steps

1. **Doors, chests and bosses** respond to Player 1 only: they drive cutscenes, room changes and item-get
   sequences that the game expects Player 1 to be in.
2. **Shared buttons.** C-button items and equipment are one save, so both players have the same item
   buttons. Per-player C buttons would need per-player save equips.
3. **Non-English A-button labels** share one name buffer per slot between the players, so Player 2's A
   label can show Player 1's word in French, German or Japanese.
4. **Rooms** are loaded around Player 1, so in dungeons Player 2 sees only the rooms Player 1 has loaded.
5. **Performance**: the world is drawn twice. The renderer work in the modern renderer branch needs to
   run its lighting and shadow passes per view.
6. Player 2 tunic colour, name tag, drop-in/drop-out.
