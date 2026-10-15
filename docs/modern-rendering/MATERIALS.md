# Materials for mod packs

A mod pack can give any texture a material: a normal map, roughness and metalness, a skin mask, or eye
shading. The material applies wherever that texture is drawn on lit geometry (characters, items,
weapons, and custom models drawn with `G_LIGHTING`), when **Per-Pixel Lighting** and **Mod Pack
Materials** are on in Settings > Graphics > Modern Rendering.

## Manifest

Put one or more JSON files under `materials/` in the pack's `.o2r`, for example `materials/child-link.json`:

```json
{
  "version": 1,
  "materials": [
    {
      "texture": "textures/player-model/child-link/basecolor",
      "normal": "textures/player-model/child-link/normal",
      "roughnessMetal": "textures/player-model/child-link/rm",
      "skinMask": "textures/player-model/child-link/skinmask"
    },
    {
      "texture": "textures/player-model/child-link/eye_basecolor",
      "normal": "textures/player-model/child-link/eye_normal",
      "shading": "eye"
    }
  ]
}
```

| Key | Meaning |
| --- | --- |
| `texture` | Resource path of the base color texture, exactly as the display list loads it. This is the key. |
| `normal` | Tangent-space normal map, OpenGL convention (+Y up the image, as in glTF), stored linear. |
| `roughnessMetal` | glTF metallic-roughness layout: G = roughness, B = metalness. |
| `skinMask` | R = how much of the surface is skin. Skin lighting wraps past the shadow line with a warm tint. |
| `shading` | `"standard"` (default) or `"eye"`. |
| `roughness`, `metalness` | Scale the map when there is one. Without a map they are the values themselves (defaults 0.5 and 0). |
| `normalStrength` | Scales the normal map's tilt (default 1). |
| `skin` | Scales the skin mask (default 1). |

Each map path is either a texture resource in the archive (RGBA32) or an image file (`.png`, `.tga`,
`.jpg`) placed in the archive as-is. Maps are sampled with the base texture's UVs, with mipmaps, and
may be any resolution (up to 8192, the upload limit).

No tangents are needed in the mesh. The shader derives the tangent frame per pixel from screen-space
derivatives.

## Eye shading

`"shading": "eye"` is meant for separate eyeball meshes. It adds a wet cornea highlight (a very smooth
clearcoat-like lobe on the eyeball's geometric normal, so it stays sharp over a bumpy iris normal map) and a
soft catchlight from the camera side, and leaves out the rim light. The normal map, if any, shapes the
iris and sclera under the cornea. The model animates the eyeballs itself.

## Changing maps at runtime

A mod can rewrite a map while the game runs (wet cloth darkening and turning glossy, mud building up).
Ship the map as an RGBA32 texture resource, edit the resource's `ImageData` in place, then call:

```c
#include <libultraship/bridge/gfxbridge.h>
GfxRefreshMaterialMap("textures/player-model/child-link/rm");
```

The renderer re-uploads that map (with fresh mipmaps) at the start of the next frame. It works for
normal, roughness-metal and skin-mask maps and is safe to call from any thread. Base color textures are
not material maps: invalidate those with `G_INVALTEXCACHE` as usual.

## Texture coordinates for large textures

Vertex texture coordinates are S10.5 fixed point, so they cover ±1024 texels. For textures of 1024 or
more texels, set a left shift on the tile (`G_TX` shift 15 doubles the range, 14 quadruples it): the
renderer honors shifts on all textures. Alternatively, author the UVs against a small proxy size and ship
the real image as an HD replacement; the renderer scales the UVs for you.

## Testing

`tools/render-test/materials.json` builds a test pack from `make-material-test-mod.py` (all generated,
no game data) and checks that Link's shield, boots, hands and eyes change when materials are on.
