# Headless Render Tests

Every modern rendering feature is checked by rendering fixed scenes with the feature on and off, without
anyone playing the game.

## How it works

`soh/soh/Enhancements/ModernRendering/RenderTest.cpp` activates when `SOH_RENDER_TEST` points at a spec.
The game skips the title screen, loads a debug save straight into each scene, waits for it to settle,
freezes the world with frame advance (actors, camera and animated textures stop), and then for each
variant sets its CVars and saves the game framebuffer (before the menu is drawn) as an image. When every
scene is done it writes `manifest.json` and exits.

`tools/render-test/compare.py` turns the captures into one sheet per scene (variants across the top,
amplified difference from the vanilla render underneath) and fails if a capture is missing or blank, or if
a feature left the image unchanged.

## Running

```sh
tools/render-test/run.sh tools/render-test/lighting-ao.json /path/to/oot.o2r build-linux
```

Needs a Linux build, `xvfb-run`, Mesa (software OpenGL) and Python with Pillow. Output goes to
`render-test-output/<spec name>/`, which is git-ignored because it contains game imagery.

## Spec format

```json
{
  "warmupTicks": 100,
  "settleTicks": 4,
  "cvars": { "applied once at boot": 1 },
  "scenes": [ { "name": "kokiri-forest", "entrance": 238, "age": "child", "dayTime": 32768,
                "pos": [0, 0, 0], "yaw": 0, "room": 0 } ],
  "variants": [ { "name": "vanilla", "cvars": { "gSettings.ModernRendering.Lighting": 0 } } ]
}
```

`entrance` is the decimal entrance index from `soh/include/tables/entrance_table.h`. `pos`, `yaw` and
`room` are optional and place Link at an exact spot.
