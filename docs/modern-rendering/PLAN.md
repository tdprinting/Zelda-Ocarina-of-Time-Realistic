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
| Ambient occlusion | Half-resolution SAO (12 spiral samples) with a depth-aware blur; GTAO later if budget allows | Big depth cue for N64 geometry at low cost |
| Volumetrics | Height fog replacing N64 fog, plus screen-space radial light shafts; froxel volumetrics later if budget allows | Light shafts are one half-res pass; froxels cost more |
| Water | Planar reflection (re-render recorded frame with a mirrored camera at reduced resolution) + SSR fallback, Fresnel, scrolling normal maps, depth-based tint and refraction | Planar is exact on OoT's flat water planes |
| Post | Bloom (dual Kawase), filmic tone map (AgX/ACES), colour grading LUT, optional sharpening | All cheap single passes |
| Performance | Render at reduced internal resolution and upscale with Snapdragon Game Super Resolution (SGSR) or FSR 1 | Qualcomm's SGSR is tuned for Adreno |
| HDR | R11G11B10F scene target | Half the bandwidth of RGBA16F |
| Glow | Emissive tag for fairies (Navi), torches, lanterns and magic, feeding bloom; soft additive sprite halo for particles | Navi and fireflies glowing is the single most recognisable "remake" cue |
| Depth of field | Half-res bokeh DOF driven by the depth texture, only in cutscenes, Z-targeting and the pause camera | Gameplay stays sharp; one pass when active |
| Reflections | Screen-space reflections on floors tagged glossy (Temple of Time, dungeon tiles, ice) at half res, falling back to the sky colour | Reuses the depth texture AO already has |

## Reference art style (Tee's UE5 remake concepts)

Tee shared four concept images of a UE5-style Ocarina of Time remake: Kokiri Forest in soft morning haze,
Link in a misty forest clearing with a glowing Navi, Link in the Temple of Time with a mirror floor and
the Master Sword, and a close-up in Hyrule Castle's garden. The rendering techniques in them, and where
each lands in the phases below:

| Seen in the references | How we get it on the Adreno 740 | Phase |
| --- | --- | --- |
| Hazy forest air, light shafts through trees | Height fog + half-res screen-space god rays from the sun/scene light | 7 |
| Soft character shadows, grounded contact | Sun shadow map with PCSS-lite soft edges, screen-space contact shadows | 4 |
| Dark crevices, grounded props | Ambient occlusion | 1 (done) |
| Glowing Navi, fireflies, lanterns | Emissive tags + bloom | 3 |
| Mirror floor in the Temple of Time | Screen-space reflections on tagged floors (planar for large flat ones) | 6 |
| Shiny sword, metal buckles, gold trim | Specular now; GGX/PBR metalness from the material map | 1 / 5 |
| Bright silhouettes against the light | Rim light | 1 (done) |
| Warm, sunlit grade, soft highlights | Filmic tone map + warm colour-grading LUT | 3 |
| Blurred foreground/background in close-ups | Depth of field in cutscenes and Z-targeting | 3 |

What shaders can't produce: most of the look in those images comes from **new high-detail art**:
sculpted faces with real eyes and hair strands, woven cloth, dense foliage and grass, detailed rocks and
tree bark. The N64 meshes have a few hundred triangles per character and 32×32/64×64 textures. Shaders
make that geometry look lit and grounded, but they can't add detail that isn't in the models.

The route for the art is Ship of Harkinian's mod format: `.o2r` packs in `mods/` that replace textures
(HD texture packs already exist for SoH) and display lists (custom models, as the community's model
replacement packs do). Suggested approach:

1. Start from an existing HD texture pack for SoH so the world texture resolution matches the lighting.
2. Add the material map (phase 5) so those packs can also ship normal, roughness and metalness maps;
   that is what makes stone, bark and metal react to light like the references.
3. Replace Link, Navi and key props first with higher-poly models built to the original skeletons (so
   animations still work), packaged as a separate model pack.
4. Foliage: denser grass and bush meshes via display-list replacement in Kokiri Forest and Hyrule
   Field first, since that is where the references differ most.

## Phases

0. **Foundation (done).** Ship of Harkinian imported with full history, libultraship vendored so the
   renderer can be changed, Linux build verified.
1. **Per-pixel lighting and ambient occlusion (this change).** Lit geometry (Link, NPCs, enemies, items,
   weapons) carries world-space normals and positions to the GPU. The shader replaces SHADE with
   per-pixel diffuse using the game's own lights, then adds specular and rim light. Screen-space ambient
   occlusion runs once the opaque geometry is drawn (the game marks that point in its display list), before
   translucent effects and the HUD. Both are tunable from the menu and overridable by mods, and both are
   covered by the headless render tests in `TESTING.md`.
2. **Android build for the Odin 2 Portal.** Gradle + SDL2 Android activity wrapping the CMake build
   (`libultraship/cmake/dependencies/android.cmake` already exists), GLES 3.2, arm64-v8a, controller
   mapping, reading `oot.o2r` from app storage.
3. **Post-processing stack.** HDR scene target, emissive tags and bloom (Navi, torches, lanterns, magic),
   filmic tone mapping, warm colour-grading LUT, depth of field for cutscenes and Z-targeting, AO under
   MSAA, SGSR/FSR 1.
4. **Frame recorder + shadows.** Record each frame's opaque draws (world-space positions are already
   available) so they can be replayed into a sun shadow map, then sample it in the lighting shader.
   This also unlocks per-pixel lighting for level geometry.
5. **Materials (started early).** Done: mod packs map a texture to normal, roughness-metal and skin
   maps plus an eye shader (`MATERIALS.md`), shaded with GGX and a per-pixel tangent frame. Still to
   come: iris parallax, an environment reflection for metals, and materials on level geometry.
6. **Water and reflections.** Tag water surfaces through the material map, planar reflections via the
   frame recorder, normal-mapped ripples, refraction and depth tint; screen-space reflections on glossy
   floors such as the Temple of Time.
7. **Volumetrics.** Height fog and sun shafts (god rays), tied to each scene's environment lighting.

Each phase lands as its own PR with a menu toggle.

## Engine limits

Raised for HD models, effects and textures (see the commit for exact values): vertices per load 128
(the GBI maximum), 1024 triangles per batch, 8192 cached textures, matrix stacks 32, 512 particle
sprites, 16/64/16 sparks/sword trails/shield particles, 128 light nodes, and a 16 MB system heap.
Save states made before this change won't load, since they copy those structures by size.

## Assets

Builds need the user's own extracted game archive (`oot.o2r`). It is never committed or uploaded;
`.gitignore` already excludes `*.o2r`, `*.otr` and ROM files.
