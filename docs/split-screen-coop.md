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

### Other engine changes

- `game.c`: controller 2 no longer drives the debug register editor while co-op is on.

## Known gaps and next steps

1. **Enemies only see Player 1.** Enemy AI aims at `GET_PLAYER()`. Plan: in `ShouldActorUpdate`, for
   hostile actors, swap in whichever player is nearer for the duration of that actor's update (the same
   context swap), and recompute `xzDistToPlayer`/`yawTowardsPlayer` against that player.
2. **Doors, chests and NPC dialogue** respond to Player 1 only, for the same reason.
3. **HUD** is shared and shows Player 1's A-button action; a per-half HUD for Player 2 is not done yet.
4. **Rooms** are loaded around Player 1, so in dungeons Player 2 sees only the rooms Player 1 has loaded.
5. **Performance**: the world is drawn twice. The renderer work in the modern renderer branch needs to
   run its lighting and shadow passes per view.
6. Player 2 tunic colour, name tag, drop-in/drop-out.
