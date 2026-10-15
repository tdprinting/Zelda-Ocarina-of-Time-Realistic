#!/usr/bin/env python3
"""Builds comparison sheets from a render test run and checks each feature actually changed the image.

Usage: compare.py <output-dir> [baseline-variant]

For every scene it writes <scene>__sheet.png: the variants side by side, with a second row showing each
variant's difference from the baseline (amplified 4x; these are analysis images, not renders). It also
writes <scene>.png, a clean full-size before/after of the baseline next to the final variant (the one named
"all", or the last one listed). summary.json records the numbers, and the script exits non-zero
when a capture is missing or blank, a non-baseline variant is identical to the baseline, or a "-neutral"
variant (a feature with its artistic terms zeroed) does not reproduce the baseline.
"""
import json
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageStat

LABEL_HEIGHT = 22


def labelled(image, text):
    canvas = Image.new("RGB", (image.width, image.height + LABEL_HEIGHT), (24, 24, 24))
    canvas.paste(image, (0, LABEL_HEIGHT))
    ImageDraw.Draw(canvas).text((6, 5), text, fill=(240, 240, 240))
    return canvas


def main():
    out_dir = Path(sys.argv[1])
    baseline = sys.argv[2] if len(sys.argv) > 2 else "vanilla"
    manifest = json.loads((out_dir / "manifest.json").read_text())

    scenes = {}
    for entry in manifest:
        scenes.setdefault(entry["scene"], []).append(entry)

    summary = {}
    failures = []
    for scene, entries in scenes.items():
        images = {}
        for entry in entries:
            path = out_dir / entry["file"]
            if not path.exists():
                failures.append(f"{scene}/{entry['variant']}: capture missing")
                continue
            image = Image.open(path).convert("RGB")
            image.save(path.with_suffix(".png"))
            path.unlink()
            images[entry["variant"]] = image
            if max(ImageStat.Stat(image).mean) < 4:
                failures.append(f"{scene}/{entry['variant']}: capture is blank")

        if baseline not in images:
            failures.append(f"{scene}: baseline '{baseline}' missing")
            continue

        base = images[baseline]
        scale = 480 / base.height if base.height > 480 else 1.0
        size = (int(base.width * scale), int(base.height * scale))
        top, bottom, stats = [], [], {}
        for name, image in images.items():
            diff = ImageChops.difference(image, base)
            gray = diff.convert("L")
            changed = sum(gray.histogram()[9:]) / (gray.width * gray.height)
            mean = sum(ImageStat.Stat(diff).mean) / 3
            stats[name] = {"meanAbsDiff": round(mean, 3), "pixelsChanged": round(changed, 4)}
            if name.endswith("-neutral"):
                # Correctness check: the feature with every artistic term at zero must reproduce the original
                if mean > 3.0:
                    failures.append(f"{scene}/{name}: differs from {baseline} by {mean:.2f} (expected a match)")
            elif name != baseline and changed == 0:
                failures.append(f"{scene}/{name}: identical to {baseline} (feature had no effect)")
            top.append(labelled(image.resize(size), name))
            amplified = diff.point(lambda v: min(255, v * 4)).resize(size)
            caption = baseline if name == baseline else f"{name} vs {baseline}: {changed:.1%} px changed"
            bottom.append(labelled(amplified, caption))

        cell_w, cell_h = top[0].width, top[0].height
        sheet = Image.new("RGB", (cell_w * len(top), cell_h * 2), (0, 0, 0))
        for i, (a, b) in enumerate(zip(top, bottom)):
            sheet.paste(a, (i * cell_w, 0))
            sheet.paste(b, (i * cell_w, cell_h))
        sheet.save(out_dir / f"{scene}__sheet.png")

        final = "all" if "all" in images else list(images)[-1]
        pair = Image.new("RGB", (base.width * 2, base.height + LABEL_HEIGHT), (24, 24, 24))
        pair.paste(labelled(base, f"{baseline} (original)"), (0, 0))
        pair.paste(labelled(images[final], f"{final} (modern rendering)"), (base.width, 0))
        pair.save(out_dir / f"{scene}.png")
        summary[scene] = stats

    (out_dir / "summary.json").write_text(json.dumps({"scenes": summary, "failures": failures}, indent=2))
    for scene, stats in summary.items():
        print(scene, json.dumps(stats))
    if failures:
        print("FAILURES:\n  " + "\n  ".join(failures))
        sys.exit(1)
    print("All captures present and every feature changed the image.")


if __name__ == "__main__":
    main()
