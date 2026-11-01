#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/thread-progress-sanitized}"
mkdir -p "$out"
"${CXX:-clang++}" -std=c++20 -O1 -g1 -fuse-ld=lld -fno-omit-frame-pointer \
  -fno-strict-aliasing -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Isrc tests/runner/ThreadProgressTests.cpp src/core/cpu/Engine.cpp \
  src/core/cpu/DecodeArm.cpp src/core/cpu/DecodeThumb.cpp src/core/cpu/DecodeVfpNeon.cpp \
  src/core/cpu/DecodeNeon.cpp src/core/memory/GuestArena.cpp -pthread -o "$out/thread-progress-tests"
"$out/thread-progress-tests"
