# Player model pipeline

Builds the high-detail young Link (`soh/assets/custom/player-model/child-link.lmdl` and its textures) from a
source `.glb`. The scripts run with Blender's Python module (`pip install bpy==4.2.0 "numpy<2" scipy pillow`
in a Python 3.11 venv). The skeleton and animations are read from your own `oot.o2r` (set `OOT_O2R`); nothing
from the game archive is written into the outputs.

Units: Blender scripts work in metres (-Y forward, Z up). From `export.py` on, everything is in Link's skeleton
units (5000 per metre, +Z forward, +Y up, +X = Link's left).

| Step | Script | Output |
| --- | --- | --- |
| 1. Sample the surface and its texture colours | `fsamp.py src.glb samp.npz` | dense coloured samples used to classify faces |
| 2. Remove the sword, back cloth and pouch, repair the holes | `clean_faces.py -- src.glb samp.npz clean.blend clean_cfg.json` | `clean.blend` |
| 3. Fit child Link's skeleton to the joint landmarks | `fit.py landmarks.json skel_fit.json` | bone positions, bind matrices, joint offsets |
| 4. Build the armature (OoT limbs + hat and skirt cloth chains) and skin weights | `rig.py -- clean.blend skel_fit.json rig.blend 0 landmarks.json` | `rig.blend` |
| 5. Clean up weights (hands, hat, skirt), split by UV, export | `export.py -- rig.blend skel_fit.json landmarks.json child-link.lmdl` | `child-link.npz` |
| 6. Face: straighten the head, eyeballs and eyelids, mouth, teeth, blendshapes | `face.py child-link.npz face.npz` | `face.npz` |
| 7. Write the game file | `write_lmdl.py face.npz skel_fit.json child-link.lmdl` | `.lmdl` v2 |
| 8. Material maps | `make_maps.py <texture prefix> maps/`, `make_eye.py maps/` | roughness/metal, skin mask, eye textures |
| 9. Wet/dirt lookup | `make_surf.py child-link.npz maps/classes.npy child-link.surf` | UV-space surface classes and heights |

Checks: `render_pose.py` poses the model with real animations (`anim:frame:azimuth`), `render_face.py` renders
blendshapes, eye gaze and eyelids, `ortho_npz.py` gives a measured front view for placing face landmarks.

In game, `tools/render-test/player-model.json` renders the model through the headless render test
(`tools/render-test/run.sh tools/render-test/player-model.json /path/to/oot.o2r build-linux`).

The `.lmdl` layout is documented in `soh/soh/Enhancements/PlayerModel/PlayerModelData.h`.
