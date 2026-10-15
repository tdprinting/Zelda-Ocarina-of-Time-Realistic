#!/usr/bin/env python3
"""Checks that the OpenGL ES build renders like the desktop OpenGL build.

Usage: compare-backends.py <gl-output-dir> <gles-output-dir>

Android runs the GLES shader path (#version 300 es, mediump defaults), which desktop runs never exercise.
Both builds capture the same spec, so every scene/variant must match closely: a shader that fails to
compile on GLES, or loses precision there, shows up as a large difference. Writes
<gles-dir>/<scene>__<variant>__backends.png (GL left, GLES right) for any capture that fails, records
the numbers in <gles-dir>/backends.json, and exits non-zero on a mismatch.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat

# Software GL and GLES differ only in rounding; a real fault moves far more than this
MAX_MEAN_DIFF = 1.5
MAX_CHANGED_FRACTION = 0.02  # pixels off by more than 24 (of 765 summed over RGB)


def main():
    gl_dir, gles_dir = Path(sys.argv[1]), Path(sys.argv[2])
    results, failed = {}, []
    captures = sorted(p for p in gl_dir.glob("*__*.png") if not p.stem.endswith(("__sheet", "__backends")))
    if not captures:
        print(f"No captures in {gl_dir}")
        return 1
    for gl_path in captures:
        gles_path = gles_dir / gl_path.name
        if not gles_path.exists():
            failed.append(gl_path.stem)
            results[gl_path.stem] = "missing"
            continue
        a = Image.open(gl_path).convert("RGB")
        b = Image.open(gles_path).convert("RGB")
        diff = ImageChops.difference(a, b)
        mean = sum(ImageStat.Stat(diff).mean) / 3
        summed = diff.convert("L").point(lambda v: 255 if v > 8 else 0)
        changed = ImageStat.Stat(summed).mean[0] / 255
        results[gl_path.stem] = {"meanAbsDiff": round(mean, 3), "pixelsChanged": round(changed, 4)}
        if mean > MAX_MEAN_DIFF or changed > MAX_CHANGED_FRACTION:
            failed.append(gl_path.stem)
            side = Image.new("RGB", (a.width * 2, a.height))
            side.paste(a, (0, 0))
            side.paste(b, (a.width, 0))
            side.save(gles_dir / f"{gl_path.stem}__backends.png")
        print(gl_path.stem, results[gl_path.stem])
    (gles_dir / "backends.json").write_text(json.dumps(results, indent=2) + "\n")
    if failed:
        print("GLES differs from GL: " + ", ".join(failed))
        return 1
    print("GLES matches GL in every capture.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
