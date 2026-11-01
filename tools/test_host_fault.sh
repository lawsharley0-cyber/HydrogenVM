#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/host-fault-sanitized}"
mkdir -p "$out"
cxx="${CXX:-clang++}"
sources=(tests/runner/HostFaultTests.cpp src/core/cpu/Engine.cpp
  src/core/cpu/DecodeArm.cpp src/core/cpu/DecodeThumb.cpp
  src/core/cpu/DecodeVfpNeon.cpp src/core/cpu/DecodeNeon.cpp
  src/core/memory/GuestArena.cpp)
"$cxx" -std=c++20 -O1 -g1 -fuse-ld=lld -fno-omit-frame-pointer -fno-strict-aliasing \
  -ffp-contract=off -fsanitize=address,undefined -fno-sanitize-recover=all \
  -Isrc "${sources[@]}" -pthread -o "$out/host-fault-tests"
# These tests deliberately raise hardware SIGSEGV. Let HVM handle it, preserving
# ASan/UBSan checks for all ordinary accesses in the test and emulator sources.
ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}handle_segv=0:handle_sigbus=0" \
  "$out/host-fault-tests"
