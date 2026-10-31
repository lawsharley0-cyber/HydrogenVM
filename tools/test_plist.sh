#!/usr/bin/env bash
# Isolated loader tests for Plist parsing.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/plist-tests}"
mkdir -p "$out"
cxx="${CXX:-g++}"
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Isrc tests/loader/PlistTests.cpp src/loader/Plist.cpp -o "$out/plist-tests"
"$out/plist-tests"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/loader/PlistTests.cpp src/loader/Plist.cpp -o "$out/plist-tests-sanitized"
"$out/plist-tests-sanitized"
