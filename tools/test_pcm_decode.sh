#!/usr/bin/env bash
# Standalone decoder regression: avoids graphics TestRunner and shared CMake ownership.
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/pcm-tests}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
host_libs=()
if [[ "$(uname -s)" == Darwin ]]; then host_libs=(-framework AudioToolbox); fi
"$cxx" -std=c++20 -ffp-contract=off -O2 -g -Wall -Wextra -Isrc tests/audio/PcmDecodeTests.cpp src/runtime/audio/Decode.cpp src/runtime/audio/MsAdpcm.cpp src/runtime/audio/HostDecode.cpp "${host_libs[@]}" -o "$out/pcm-tests"
"$out/pcm-tests"
"$cxx" -std=c++20 -ffp-contract=off -O1 -g -fsanitize=address,undefined,float-cast-overflow -fno-sanitize-recover=all -fno-omit-frame-pointer -Isrc tests/audio/PcmDecodeTests.cpp src/runtime/audio/Decode.cpp src/runtime/audio/MsAdpcm.cpp src/runtime/audio/HostDecode.cpp "${host_libs[@]}" -o "$out/pcm-tests-sanitized"
"$out/pcm-tests-sanitized"
