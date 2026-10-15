# Modern Animation

Smoother, more natural movement for every animated character (Link, NPCs and enemies), modelled on how
Twilight Princess and Ocarina of Time 3D move their characters. Settings live under
**Enhancements > Graphics > Modern Animation** and are on by default.

Only what is drawn changes. Joint tables, animation frames and timing, actor positions, and everything the
game reads back from limb matrices (sword and claw hitboxes, focus points, effect positions) stay exactly as
in the original. The test harness checks this frame by frame: every actor's position, rotation, velocity,
health and focus, Link's state and sword, and every registered hitbox must match the original in every frame
of every clip.

## What the reference games do

| | Ocarina of Time (N64) | Twilight Princess (GameCube, Dusklight decomp) | This build |
|---|---|---|---|
| Blending between animations | Per-axis Euler lerp (`SkelAnime_InterpFrameTable`) | Quaternion blend (`mDoExt_McaMorf::calc`: `JMAEulerToQuat` + `JMAQuatLerp`) | Game's own morphs unchanged; added cross-fades and sub-frame blends use quaternion slerp |
| Transition curve | Linear | Linear, or an accelerating step (`mDoExt_morf_c::frameUpdate`) | Ease-in-out (smoothstep) |
| Instant animation cuts | Common (`morphFrames == 0`) for NPCs, enemies and many of Link's actions | Almost every change passes a morph time (`setAnm(..., i_morf)`) | Automatic cross-fade (Link 2 frames, others 3) |
| Between keyframes | Holds each keyframe unless the actor asked for `ANIMMODE_*_INTERP` | Hermite-interpolated keys (J3D) | Always samples between keyframes at fractional play speeds |
| NPC look-at | Head and torso, exponential approach (`Math_SmoothStepToS`) | Head, neck and backbone, smoothed (`daNpcT_JntAnm_c::calcJntRad`) | Unchanged (it drives NPC facing, which is gameplay state) |

Ocarina of Time 3D re-animated much of the cast by hand with smoother, more expressive motion; that is
asset work, not engine behaviour, so the engine side here follows Twilight Princess.

## How it works

`soh/soh/Enhancements/ModernAnimation/ModernAnimation.cpp`, hooked from `z_skelanime.c`, with a display
matrix stack in `sys_matrix.c`:

- **Automatic cross-fades.** When `Animation_Change`/`LinkAnimation_Change` cut instantly to a different
  animation, the pose on screen is saved and the drawn pose fades from it into the new animation over a few
  frames (Link 2, others 3) along a smoothstep curve, limb rotations only.
- **Smooth slow animations.** Animations played below full speed (walks scaled by velocity, idles at half
  speed) are drawn blended toward the next keyframe by the fractional frame instead of holding each keyframe
  for several frames.
- **Drawing only.** At the end of each game frame, once every actor and Link's animation queue have
  updated, each affected skeleton gets a per-limb rotation that takes the original pose to the smoothed one.
  While that skeleton draws, every matrix operation runs on two stacks: the normal one with the original
  limb rotations, which is what limb callbacks and hitbox code read, and a display stack with the smoothed
  rotations, which is what goes to the GPU and to the 60 fps frame interpolation. The game itself never
  sees the smoothed pose. Items an actor draws by copying the current matrix itself sit on the original pose
  for that frame.

The game's own morphs, NPC head tracking and animation timing are untouched, because they feed gameplay
state. State is dropped when a skeleton is re-initialised or freed, or its actor is destroyed; while the
game is paused the last smoothed pose is kept.

Link's sword trails, NPC eye textures and blinking are untouched. NPC eyes in Ocarina of Time are single
textures (no separate pupil), so Twilight Princess style eye look-at, which slides the pupil texture, would
need new eye textures or a shader.

## Testing

`tools/anim-test/run.sh tools/anim-test/clips.json /path/to/oot.o2r build-linux` boots the game headless,
plays each clip with scripted controller input once per variant (same seed, full health), saves every
frame, per-frame limb angular acceleration and a hash of the gameplay state, then writes to
`anim-test-output/clips/`: side-by-side videos (real time and slowed down), a GIF of each clip's highlight,
and the largest limb angular acceleration in each animation transition, compared at the same moments in
every variant.

Results from `clips.json` (degrees/frame², lower is smoother):

| | transitions | original mean / 90th pct / max | modern mean / 90th pct / max |
|---|---|---|---|
| Link | 49 | 35.3 / 91.8 / 127.5 | 29.2 / 63.3 / 87.8 |
| NPCs and enemies | 38 | 30.3 / 91.3 / 95.5 | 19.5 / 43.7 / 94.0 |

Gameplay was identical to the original in all 380 frames of the three clips, including the Stalfos fight.

## Expressive NPC faces

`NpcFace.cpp` gives every NPC, enemy and boss a face state each frame, read from the game without changing
it (its own random generator, no actor writes):

- **Mood from what they are doing.** About 100 animations are tagged with a mood: the rock-lifting and
  grass-cutting Kokiri strain in beats with the motion, Mido and the punching Kokiri are angry, laughing and
  dancing characters squint with joy, Talon sleeps, the windmill man scowls, Ruto squirms. Without a tagged
  animation the situation decides: talking to Link (pleased), Link close with a sword out (worried), Link
  arriving after a while (a moment of surprise), enemies facing Link (focused), taking a hit (pain).
- **Blinks** every 2 to 5 seconds, sometimes double, slower and more often when tired or bored, and often with
  a big eye movement.
- **Gaze** at Link's head when he is in view or talking to them, with small saccades; idle characters glance
  around, and busy ones keep their eyes on their work.

Each mood is a lid shape (upper and lower lid, brow slant toward the nose, pupil size). The renderer's eye
shader draws it from `gSPSetFaceParams` (a `GfxFaceParams`, emitted around each actor's draw on both display
lists). `tools/anim-test/faces.json` renders two Kokiri in every mood (`gSettings.ModernAnimation.NpcFaceForceMood`
forces one mood, for testing) and checks gameplay is unchanged.
