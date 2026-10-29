#!/usr/bin/env bash
# Isolated loader tests: does not edit graphics TestRunner or shared CMake targets.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/macho-probe-tests}"
mkdir -p "$out"
cxx="${CXX:-g++}"
"$cxx" -std=c++20 -O2 -g -Wall -Wextra -Isrc tests/loader/MachOProbeTests.cpp src/loader/MachO.cpp src/core/memory/GuestArena.cpp -o "$out/macho-probe-tests"
"$out/macho-probe-tests"
"$cxx" -std=c++20 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/loader/MachOProbeTests.cpp src/loader/MachO.cpp src/core/memory/GuestArena.cpp -o "$out/macho-probe-tests-sanitized"
"$out/macho-probe-tests-sanitized"
