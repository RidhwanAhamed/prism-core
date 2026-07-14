#!/bin/bash
# Build prism_core.xcframework (Task 7): device arm64 + simulator arm64 dynamic
# frameworks from the repo's own CMake tree, composed with xcodebuild. Output lands in
# core/ios/prism_core.xcframework (gitignored build artifact; the smoke app's podspec
# vendors it from there).
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
COMMON=(
  -G Ninja
  -DCMAKE_SYSTEM_NAME=iOS
  -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
  -DCMAKE_BUILD_TYPE=Release
  -DPRISM_BUILD_SHARED=ON
  -DPRISM_BUILD_TESTS=OFF
  -DPRISM_BUILD_HARNESS=OFF
  -DPRISM_BUILD_TOOLS=OFF
)

cmake -B "$ROOT/build/ios-device" -S "$ROOT" "${COMMON[@]}" \
  -DCMAKE_OSX_SYSROOT=iphoneos -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$ROOT/build/ios-device"

cmake -B "$ROOT/build/ios-sim" -S "$ROOT" "${COMMON[@]}" \
  -DCMAKE_OSX_SYSROOT=iphonesimulator -DCMAKE_OSX_ARCHITECTURES=arm64
cmake --build "$ROOT/build/ios-sim"

OUT="$ROOT/core/ios/prism_core.xcframework"
rm -rf "$OUT"
mkdir -p "$ROOT/core/ios"
xcodebuild -create-xcframework \
  -framework "$ROOT/build/ios-device/core/prism_core.framework" \
  -framework "$ROOT/build/ios-sim/core/prism_core.framework" \
  -output "$OUT"

echo "xcframework at $OUT"
