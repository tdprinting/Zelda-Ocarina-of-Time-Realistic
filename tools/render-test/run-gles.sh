#!/usr/bin/env bash
# Runs a render test spec on both the desktop OpenGL build and an OpenGL ES build, then checks that they
# match. The GLES build exercises the shader path Android uses.
#
#   tools/render-test/run-gles.sh <spec.json> <path/to/oot.o2r> [gl-build-dir] [gles-build-dir] [out-dir]
#
# Configure the GLES build with: cmake -S . -B build-gles -G Ninja -DUSE_OPENGLES=ON
# and build it with: cmake --build build-gles --target soh GenerateSohOtr
set -euo pipefail

HERE=$(dirname "$(realpath "$0")")
SPEC=$1
OOT_O2R=$2
GL_BUILD=${3:-build-linux}
GLES_BUILD=${4:-build-gles}
OUT_DIR=${5:-render-test-output/$(basename "$SPEC" .json)}

"$HERE/run.sh" "$SPEC" "$OOT_O2R" "$GL_BUILD" "$OUT_DIR"
"$HERE/run.sh" "$SPEC" "$OOT_O2R" "$GLES_BUILD" "$OUT_DIR-gles"
python3 "$HERE/compare-backends.py" "$OUT_DIR" "$OUT_DIR-gles"
