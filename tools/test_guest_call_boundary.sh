#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/guest-call-sanitized}"
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g1 -fuse-ld=lld -fno-omit-frame-pointer \
  -fno-strict-aliasing -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Isrc tests/runner/GuestCallBoundaryTests.cpp src/core/cpu/Engine.cpp \
  src/core/cpu/DecodeArm.cpp src/core/cpu/DecodeThumb.cpp src/core/cpu/DecodeVfpNeon.cpp \
  src/core/cpu/DecodeNeon.cpp src/core/memory/GuestArena.cpp -pthread -o "$out/guest-call-tests"
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}handle_segv=0:handle_sigbus=0" "$out/guest-call-tests"
