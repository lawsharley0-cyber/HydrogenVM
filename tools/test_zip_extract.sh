#!/usr/bin/env bash
# Isolated loader tests for extraction
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/zip-extract-tests}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Isrc tests/loader/ZipExtractTests.cpp src/loader/Zip.cpp -lz -o "$out/zip-extract"
"$out/zip-extract" "$out/fixtures"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/loader/ZipExtractTests.cpp src/loader/Zip.cpp -lz -o "$out/zip-extract-sanitized"
"$out/zip-extract-sanitized" "$out/fixtures-sanitized"
