#!/usr/bin/env bash
# Headless render test: boots the game under Xvfb with Mesa's software GL, captures every scene/variant
# listed in the spec, then builds comparison sheets.
#
#   tools/render-test/run.sh <spec.json> <path/to/oot.o2r> [build-dir] [out-dir]
#
# The game archive is only symlinked into a temporary run directory; nothing is copied into the repo.
set -euo pipefail

SPEC=$(realpath "$1")
OOT_O2R=$(realpath "$2")
BUILD_DIR=$(realpath "${3:-build-linux}")
OUT_DIR=$(realpath -m "${4:-render-test-output/$(basename "$SPEC" .json)}")
HERE=$(dirname "$(realpath "$0")")

RUN_DIR=$(mktemp -d)
trap 'rm -rf "$RUN_DIR"' EXIT
mkdir -p "$OUT_DIR"
rm -f "$OUT_DIR"/*.ppm "$OUT_DIR"/manifest.json

cp "$BUILD_DIR/soh/soh.elf" "$BUILD_DIR/soh/soh.o2r" "$RUN_DIR/"
ln -s "$OOT_O2R" "$RUN_DIR/oot.o2r"
# The startup check expects the extractor's assets folder next to the executable even when archives exist
mkdir -p "$RUN_DIR/assets"

# Point the spec's output at OUT_DIR
python3 -c 'import json,sys; s=json.load(open(sys.argv[1])); s["output"]=sys.argv[2]; json.dump(s,open(sys.argv[3],"w"),indent=2)' \
    "$SPEC" "$OUT_DIR" "$RUN_DIR/spec.json"

(
    cd "$RUN_DIR"
    SHIP_HOME="$RUN_DIR" SOH_RENDER_TEST="$RUN_DIR/spec.json" \
    LIBGL_ALWAYS_SOFTWARE=1 SDL_AUDIODRIVER=dummy \
    timeout "${RENDER_TEST_TIMEOUT:-900}" \
    xvfb-run -a -s "-screen 0 1280x960x24" ./soh.elf > "$OUT_DIR/game.log" 2>&1
) || { echo "Game exited with an error; see $OUT_DIR/game.log"; tail -20 "$OUT_DIR/game.log"; exit 1; }

python3 "$HERE/compare.py" "$OUT_DIR"
