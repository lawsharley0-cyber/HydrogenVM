#!/usr/bin/env bash
# Isolated loader tests: does not edit graphics TestRunner or shared CMake targets.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/zip-tests}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Isrc tests/loader/ZipBoundsTests.cpp src/loader/Zip.cpp -lz -o "$out/zip-tests"
"$out/zip-tests" "$out/fixtures"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/loader/ZipBoundsTests.cpp src/loader/Zip.cpp -lz -o "$out/zip-tests-sanitized"
"$out/zip-tests-sanitized" "$out/fixtures-sanitized"
