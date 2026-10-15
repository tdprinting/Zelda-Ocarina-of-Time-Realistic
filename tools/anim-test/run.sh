#!/usr/bin/env bash
# Headless animation test: boots the game under Xvfb with Mesa's software GL, plays every clip in the spec
# once per variant with scripted input, then builds side-by-side videos and a smoothness summary.
#
#   tools/anim-test/run.sh <spec.json> <path/to/oot.o2r> [build-dir] [out-dir]
#
# The game archive is only symlinked into a temporary run directory; nothing is copied into the repo.
set -euo pipefail

SPEC=$(realpath "$1")
OOT_O2R=$(realpath "$2")
BUILD_DIR=$(realpath "${3:-build-linux}")
OUT_DIR=$(realpath -m "${4:-anim-test-output/$(basename "$SPEC" .json)}")
HERE=$(dirname "$(realpath "$0")")

RUN_DIR=$(mktemp -d)
trap 'rm -rf "$RUN_DIR"' EXIT
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.ppm "$OUT_DIR"/manifest.json

cp "$BUILD_DIR/soh/soh.elf" "$BUILD_DIR/soh/soh.o2r" "$RUN_DIR/"
ln -s "$OOT_O2R" "$RUN_DIR/oot.o2r"
mkdir -p "$RUN_DIR/assets" "$RUN_DIR/mods"

SIZE=${ANIM_TEST_SIZE:-640x480}
printf '{"Window":{"Width":%d,"Height":%d}}\n' "${SIZE%x*}" "${SIZE#*x}" > "$RUN_DIR/shipofharkinian.json"

python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); s["output"]=sys.argv[2]; json.dump(s,open(sys.argv[3],"w"),indent=2)' \
    "$SPEC" "$OUT_DIR" "$RUN_DIR/spec.json"
cp "$RUN_DIR/spec.json" "$OUT_DIR/spec.json"

(
    cd "$RUN_DIR"
    SHIP_HOME="$RUN_DIR" SOH_ANIM_TEST="$RUN_DIR/spec.json" \
    LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy \
    timeout "${ANIM_TEST_TIMEOUT:-1800}" \
    xvfb-run -a -s "-screen 0 1920x1440x24" ./soh.elf > "$OUT_DIR/game.log" 2>&1
) || { echo "Game exited with an error; see $OUT_DIR/game.log"; tail -20 "$OUT_DIR/game.log"; exit 1; }

python3 "$HERE/make-videos.py" "$OUT_DIR"
