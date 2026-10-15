# Modern Rendering Plan

Goal: take Ship of Harkinian's renderer and layer modern real-time rendering on top of the vanilla game:
per-pixel lighting, real shadows, volumetric light and fog, material shaders for characters, items and
weapons, and water with reflections. The target device is the **AYN Odin 2 Portal** (Snapdragon 8 Gen 2,
Adreno 740, Android, OpenGL ES 3.2 / Vulkan 1.3).

Every feature is opt-in from **Settings > Graphics > Modern Rendering** and falls back to the vanilla look
when turned off.

## How the renderer works today

Ship of Harkinian runs the original game code, which emits N64 display lists. libultraship's Fast3D
interpreter (`libultraship/src/fast/interpreter.cpp`) executes those lists on the CPU:

- Vertices are transformed to clip space on the CPU. The N64 lighting model (one ambient plus up to 7
  directional lights) is evaluated **per vertex** on the CPU and baked into the SHADE color.
- Triangles are batched and handed to a backend (`backends/gfx_opengl.cpp` on Android) together with a
  shader generated from the N64 color combiner.
- Shader templates (`libultraship/src/fast/shaders/opengl/default.shader.glsl`) are loaded as resources
  from the `.o2r` archives, so **mod packs can replace them**.

OoT folds the camera's view matrix into the projection matrix, so the model matrix on the stack maps
straight to world space. That gives us world-space normals and positions for free, and the camera
position can be recovered from the view-projection matrix.

## Engine vs. mods

Tee prefers features as Ship of Harkinian mods. The split:

| Layer | Lives in | What it provides |
| --- | --- | --- |
| Engine hooks | `libultraship` (vendored here) | Data the vanilla pipeline throws away: normals, world positions, light uniforms, depth, a frame recorder for extra passes, render targets |
| Default look | shader templates bundled in `soh.o2r` | The lighting, shadow, water and post shaders |
| Mod packs | `.o2r` files in `mods/` | Override any shader template, ship HD/PBR textures and the material map |

So the engine is changed once to expose the data, and the actual look is a moddable shader layer.

## Techniques chosen for a mobile tile-based GPU

Adreno 740 is a tile-based GPU: bandwidth and render-target switches are expensive, ALU is cheap. The
choices below favour forward shading, few full-screen passes and half-resolution effects.

| Area | Technique | Why it fits |
| --- | --- | --- |
| Lighting | Forward per-pixel lighting with normalized Blinn-Phong, moving to GGX/PBR once materials exist | No G-buffer bandwidth; OoT has at most 7 lights per draw |
| Materials | Texture-path → material map (JSON in a mod pack): roughness, metalness, emissive, normal map, "water", "skin", "metal" | SoH knows each texture's asset path, so materials can be tagged without touching game code |
| Shadows | Sun shadow map with 2 cascades, PCF + small blocker search (PCSS-lite); contact shadows in screen space for small details | Cascades keep resolution near the camera; 2 cascades is the mobile sweet spot |
| Ambient occlusion | Half-resolution GTAO with bilateral upsample | Big depth cue for N64 geometry at low cost |
| Volumetrics | Height fog replacing N64 fog, plus screen-space radial light shafts; froxel volumetrics later if budget allows | Light shafts are one half-res pass; froxels cost more |
| Water | Planar reflection (re-render recorded frame with a mirrored camera at reduced resolution) + SSR fallback, Fresnel, scrolling normal maps, depth-based tint and refraction | Planar is exact on OoT's flat water planes |
| Post | Bloom (dual Kawase), filmic tone map (AgX/ACES), colour grading LUT, optional sharpening | All cheap single passes |
| Performance | Render at reduced internal resolution and upscale with Snapdragon Game Super Resolution (SGSR) or FSR 1 | Qualcomm's SGSR is tuned for Adreno |
| HDR | R11G11B10F scene target | Half the bandwidth of RGBA16F |

## Phases

0. **Foundation (done).** Ship of Harkinian imported with full history, libultraship vendored so the
   renderer can be changed, Linux build verified.
1. **Per-pixel lighting (this change).** Lit geometry (Link, NPCs, enemies, items, weapons) carries
   world-space normals and positions to the GPU. The shader replaces SHADE with per-pixel diffuse using
   the game's own lights, then adds specular and rim light. Tunable from the menu, overridable by mods.
2. **Android build for the Odin 2 Portal.** Gradle + SDL2 Android activity wrapping the CMake build
   (`libultraship/cmake/dependencies/android.cmake` already exists), GLES 3.2, arm64-v8a, controller
   mapping, reading `oot.o2r` from app storage.
3. **Post-processing stack.** HDR scene target, bloom, tone mapping, colour grading, GTAO, SGSR/FSR 1.
4. **Frame recorder + shadows.** Record each frame's opaque draws (world-space positions are already
   available) so they can be replayed into a sun shadow map, then sample it in the lighting shader.
   This also unlocks per-pixel lighting for level geometry.
5. **Materials.** Material map format, HD texture pack hooks for normal/roughness maps, GGX shading.
6. **Water.** Tag water surfaces through the material map, planar reflections via the frame recorder,
   normal-mapped ripples, refraction and depth tint.
7. **Volumetrics.** Height fog and sun shafts, tied to each scene's environment lighting.

Each phase lands as its own PR with a menu toggle.

## Assets

Builds need the user's own extracted game archive (`oot.o2r`). It is never committed or uploaded;
`.gitignore` already excludes `*.o2r`, `*.otr` and ROM files.
