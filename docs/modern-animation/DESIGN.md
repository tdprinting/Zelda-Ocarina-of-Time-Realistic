# Modern Animation

Smoother, more natural movement for every animated character (Link, NPCs and enemies), modelled on how
Twilight Princess and Ocarina of Time 3D move their characters. Settings live under
**Enhancements > Graphics > Modern Animation** and are on by default. Only limb rotations change: animation
timing, frame counters and the root translation that moves actors are computed exactly as in the original,
so movement is unchanged (the test harness checks Link ends every clip in the same spot with the features on
and off). Hitboxes attached to limbs (sword and claw colliders) are taken from the drawn limb matrices, as in
the original, so during a cross-fade they follow the smoothed pose and a hit can connect a frame earlier or
later; in the enemy test clip that is enough to send a Stalfos down a different branch of its AI.

## What the reference games do

| | Ocarina of Time (N64) | Twilight Princess (GameCube, Dusklight decomp) | This build |
|---|---|---|---|
| Blending between animations | Per-axis Euler lerp (`SkelAnime_InterpFrameTable`) | Quaternion blend (`mDoExt_McaMorf::calc`: `JMAEulerToQuat` + `JMAQuatLerp`) | Quaternion slerp |
| Transition curve | Linear | Linear, or an accelerating step (`mDoExt_morf_c::frameUpdate`) | Ease-in-out (smoothstep) |
| Instant animation cuts | Common (`morphFrames == 0`) for NPCs, enemies and many of Link's actions | Almost every change passes a morph time (`setAnm(..., i_morf)`) | Automatic cross-fade (Link 2 frames, others 3) |
| Between keyframes | Holds each keyframe unless the actor asked for `ANIMMODE_*_INTERP` | Hermite-interpolated keys (J3D) | Always samples between keyframes at fractional play speeds |
| NPC look-at | Head and torso, exponential approach (`Math_SmoothStepToS`) | Head, neck and backbone, smoothed (`daNpcT_JntAnm_c::calcJntRad`) | Same joints, critically damped spring (eases in and out) |

Ocarina of Time 3D re-animated much of the cast by hand with smoother, more expressive motion; that is
asset work, not engine behaviour, so the engine side here follows Twilight Princess.

## How it works

`soh/soh/Enhancements/ModernAnimation/ModernAnimation.cpp`, hooked from `z_skelanime.c` and `z_actor.c`:

- **Natural joint rotation.** `SkelAnime_InterpFrameTable` (every morph, partial-frame blend and Link's
  upper/lower body blend) converts each limb's ZYX Euler angles to a quaternion, slerps, and converts back,
  choosing the Euler solution nearest the original lerp so the 60 fps frame interpolation (which blends Euler
  angles per component) stays continuous. Entry 0, the root translation, is lerped as before.
- **Eased transitions.** The game's own morphs (`SkelAnime_Morph`, `LinkAnimation_Morph` and the negative
  "start immediately" morphs) keep their length but follow a smoothstep curve instead of a linear one.
- **Automatic cross-fades.** When `Animation_Change`/`LinkAnimation_Change` cut instantly to a different
  animation, the pose on screen is saved and the new animation fades in from it over a few frames, limb
  rotations only. The new animation starts on the same frame as in the original, so `Animation_OnFrame`
  checks and "animation finished" results are unaffected. Skeletons that stop updating mid-fade are finished
  at the end of the frame from stored poses. State is dropped when a skeleton is re-initialised or freed, or
  its actor is destroyed.
- **Smooth slow animations.** Animations played below full speed (walks scaled by velocity, idles at
  half speed) blend toward the next keyframe by the fractional frame instead of holding each keyframe for
  several frames.
- **Smooth head tracking.** `Npc_TrackPointWithLimits` and `Actor_TrackPoint`/`Actor_TrackNone` (used by
  about 50 NPCs and enemies) use a critically damped spring with the original speed cap, so heads and torsos
  start and stop turning gently instead of snapping into motion.

Link's sword trails, NPC eye textures and blinking are untouched. NPC eyes in Ocarina of Time are single
textures (no separate pupil), so Twilight Princess style eye look-at, which slides the pupil texture, would
need new eye textures or a shader.

## Testing

`tools/anim-test/run.sh tools/anim-test/clips.json /path/to/oot.o2r build-linux` boots the game headless,
plays each clip with scripted controller input once per variant (same seed), saves every frame and
per-frame limb angular acceleration, then writes side-by-side GIFs and `summary.md` to
`anim-test-output/clips/`: side-by-side videos (real time and slowed down), a GIF of each clip's highlight,
and the largest limb angular acceleration in each animation transition, compared at the same moments in
every variant.

Results from `clips.json` (degrees/frame², lower is smoother):

| | transitions | original mean / 90th pct | modern mean / 90th pct |
|---|---|---|---|
| Link | 49 | 35.3 / 91.8 | 32.7 / 67.5 |
| NPCs and enemies | 38 | 30.3 / 91.3 | 20.1 / 45.2 |

Link's single largest value rises (127 to 167) at the peak of his first sword swing: sub-frame sampling
plays the swing at its true even rate instead of the original's alternating 1- and 2-keyframe steps (Link's
animations advance 1.5 keyframes per game frame), so the fastest part of the swing is no longer skipped.
