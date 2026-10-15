#!/usr/bin/env python3
"""Builds side-by-side videos and a smoothness summary from an animation test run.

    make-videos.py <out-dir>

Writes, per clip: <clip>.mp4 (every variant next to each other, labelled, real time at the game's 20 Hz),
<clip>-slow.mp4 (the clip's "highlight" frame range at quarter speed) and <clip>.gif (the highlight at half
speed, small enough to post). Also summary.json and summary.md. Fails if a capture is missing or blank, or if
Link ends a clip in a different place in any variant (the features must not change gameplay).
"""
import json
import math
import shutil
import subprocess
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

from PIL import Image, ImageDraw, ImageStat

TILE_WIDTH = 480
LABEL_HEIGHT = 28


def sheet_for(tiles):
    h = tiles[0][1].height
    sheet = Image.new("RGB", (TILE_WIDTH * len(tiles), h + LABEL_HEIGHT), (16, 16, 16))
    draw = ImageDraw.Draw(sheet)
    for i, (label, tile) in enumerate(tiles):
        sheet.paste(tile, (i * TILE_WIDTH, LABEL_HEIGHT))
        draw.text((i * TILE_WIDTH + 10, 8), label, fill=(235, 235, 235))
    return sheet


def encode(frames, path, fps):
    with tempfile.TemporaryDirectory() as tmp:
        for i, f in enumerate(frames):
            f.save(f"{tmp}/{i:04d}.png")
        subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-framerate", str(fps), "-i", f"{tmp}/%04d.png",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-vf",
                        "pad=ceil(iw/2)*2:ceil(ih/2)*2", str(path)], check=True)


def percentile(values, p):
    if not values:
        return 0.0
    values = sorted(values)
    return values[min(len(values) - 1, int(len(values) * p / 100.0))]


def main():
    out = Path(sys.argv[1])
    manifest = json.loads((out / "manifest.json").read_text())
    spec = json.loads((out / "spec.json").read_text()) if (out / "spec.json").exists() else {"clips": []}
    highlights = {c["name"]: c["highlight"] for c in spec["clips"] if "highlight" in c}
    captures = defaultdict(lambda: defaultdict(dict))
    variants = []
    for c in manifest["captures"]:
        captures[c["clip"]][c["variant"]][c["frame"]] = out / c["file"]
        if c["variant"] not in variants:
            variants.append(c["variant"])

    failures = []
    for clip, by_variant in captures.items():
        frames = sorted(set.union(*(set(v.keys()) for v in by_variant.values())))
        sheets = []
        for f in frames:
            tiles = []
            for v in variants:
                path = by_variant[v].get(f)
                if path is None or not path.exists():
                    failures.append(f"missing capture {clip} {v} {f}")
                    continue
                img = Image.open(path).convert("RGB")
                if max(ImageStat.Stat(img).stddev) < 1.0:
                    failures.append(f"blank capture {path.name}")
                h = round(img.height * TILE_WIDTH / img.width)
                tiles.append((f"{v}   frame {f}", img.resize((TILE_WIDTH, h), Image.LANCZOS)))
            if len(tiles) == len(variants):
                sheets.append((f, sheet_for(tiles)))
        if not sheets:
            continue
        encode([s for _, s in sheets], out / f"{clip}.mp4", 20)
        lo, hi = highlights.get(clip, (frames[0], frames[-1]))
        slow = [s for f, s in sheets if lo <= f <= hi]
        encode(slow, out / f"{clip}-slow.mp4", 5)
        small = [s.resize((s.width * 2 // 3, s.height * 2 // 3), Image.LANCZOS).quantize(colors=128)
                 for s in slow]
        small[0].save(out / f"{clip}.gif", save_all=True, append_images=small[1:], duration=100, loop=0)

    # Smoothness on animation transitions: the largest limb angular acceleration (degrees/frame^2) of each
    # skeleton in the frame it changed animation and the two after, compared at the same moments in every
    # variant (runs are deterministic, so skeleton entries line up).
    accel = defaultdict(dict)
    transitions = set()
    for s in manifest["stats"]:
        for aid, nth, a, changed in s["skels"]:
            key = (s["clip"], aid, nth, s["frame"])
            accel[s["variant"]][key] = a
            if changed and s["variant"] == variants[0]:
                transitions.add(key)
    summary = {}
    for who, is_link in (("Link", True), ("NPCs and enemies", False)):
        keys = [k for k in transitions if (k[1] == 0) == is_link]
        if not keys:
            continue
        summary[who] = {}
        for v in variants:
            values = [accel[v].get(k, 0.0) for k in keys]
            summary[who][v] = {"samples": len(values), "mean": round(sum(values) / len(values), 1),
                               "p90": round(percentile(values, 90), 1), "max": round(max(values), 1)}

    # Link must end up in the same place whatever the variant: the features are visual only
    last = {}
    for s in manifest["stats"]:
        last[(s["clip"], s["variant"])] = s["playerPos"]
    for clip in captures:
        ref = last.get((clip, variants[0]))
        for v in variants[1:]:
            pos = last.get((clip, v))
            if ref and pos and math.dist(ref, pos) > 1.0:
                failures.append(f"{clip}: Link ended {math.dist(ref, pos):.1f} units away in {v}")

    (out / "summary.json").write_text(json.dumps(summary, indent=2))
    lines = ["Limb angular acceleration during animation transitions (degrees/frame^2, lower is smoother)", "",
             "| who | variant | transitions | mean | 90th percentile | max |", "|---|---|---|---|---|---|"]
    for who, by_variant in summary.items():
        for v, s in by_variant.items():
            lines.append(f"| {who} | {v} | {s['samples']} | {s['mean']} | {s['p90']} | {s['max']} |")
    (out / "summary.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    if failures:
        print("\n".join(failures))
        sys.exit(1)


if __name__ == "__main__":
    if shutil.which("ffmpeg") is None:
        sys.exit("ffmpeg is required")
    main()
