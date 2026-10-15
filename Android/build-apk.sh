#!/usr/bin/env bash
# Builds the Android APK (arm64-v8a) for the AYN Odin 2 Portal and other Android 8+ devices.
#
#   Android/build-apk.sh [debug|release]      (default: release)
#
# Needs ANDROID_HOME (or ANDROID_SDK_ROOT) pointing at an SDK with NDK 28.2.13676358 and CMake 4.1.2,
# plus a host C++ toolchain for the asset tools. The APK lands in Android/app/build/outputs/apk/.
# It never contains ROM assets: players pick the oot.o2r they made themselves on first launch.
set -euo pipefail

variant="${1:-release}"
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/.." && pwd)"
host_build="${HOST_BUILD_DIR:-$root/build-android-host}"

# 1. soh.o2r: the asset tools have to run on this machine, so build them with a host configure.
#    Set SOH_O2R to reuse one that's already built (CI passes the generate-soh-otr artifact).
if [[ -z "${SOH_O2R:-}" ]]; then
    cmake -S "$root" -B "$host_build" -G Ninja -DSOH_TOOLS_ONLY=ON -DCMAKE_BUILD_TYPE=Release
    cmake --build "$host_build" --target GenerateSohOtr
    SOH_O2R="$host_build/soh/soh.o2r"
fi
mkdir -p "$here/app/src/main/assets"
cp "$SOH_O2R" "$here/app/src/main/assets/soh.o2r"

# 2. the APK; gradle drives the NDK build of the game itself
cd "$here"
task="assemble$(tr '[:lower:]' '[:upper:]' <<< "${variant:0:1}")${variant:1}"
./gradlew --no-daemon "$task"
ls -l app/build/outputs/apk/"$variant"/*.apk
