#!/usr/bin/env bash
# Isolated loader tests: does not edit graphics TestRunner or shared CMake targets.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/app-bundle-tests}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
sources=(
  tests/loader/AppBundleReadTests.cpp
  src/loader/AppBundle.cpp
  src/loader/Plist.cpp
  src/loader/Zip.cpp
  src/loader/MachO.cpp
  src/loader/ElfLoader.cpp
  src/core/memory/GuestArena.cpp
)
echo "Building AppBundleReadTests..."
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Wno-unused-parameter -Isrc \
  "${sources[@]}" -lz -pthread -o "$out/app-bundle-tests"
echo "Running AppBundleReadTests..."
"$out/app-bundle-tests"
echo "Building AppBundleReadTests (ASan)..."
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all \
  -fno-omit-frame-pointer -Isrc -Wno-unused-parameter \
  "${sources[@]}" -lz -pthread -o "$out/app-bundle-tests-sanitized"
echo "Running AppBundleReadTests (ASan)..."
"$out/app-bundle-tests-sanitized"
