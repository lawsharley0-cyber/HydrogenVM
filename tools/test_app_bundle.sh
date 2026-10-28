#!/usr/bin/env bash
# Isolated loader tests: does not edit graphics TestRunner or shared CMake targets.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/app-bundle-tests}"
mkdir -p "$out"
cxx="${CXX:-g++}"

# The core source files needed by the loader to build and test AppBundle.
# Note: we need core cpu/memory sources since MachO pulls in GuestArena and Decoder for patching, etc.
SOURCES="tests/loader/AppBundleTests.cpp src/loader/AppBundle.cpp src/loader/Plist.cpp src/loader/Zip.cpp src/loader/MachO.cpp src/loader/ElfLoader.cpp src/core/memory/GuestArena.cpp"

echo "Building AppBundleTests..."
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Isrc -Wno-unused-parameter \
  -ffp-contract=off \
  $SOURCES -lz -o "$out/app-bundle-tests"
echo "Running AppBundleTests..."
"$out/app-bundle-tests"

echo "Building AppBundleTests (ASan)..."
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc -Wno-unused-parameter \
  -ffp-contract=off \
  $SOURCES -lz -o "$out/app-bundle-tests-sanitized"
echo "Running AppBundleTests (ASan)..."
"$out/app-bundle-tests-sanitized"
