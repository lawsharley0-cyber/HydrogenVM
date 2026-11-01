#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/thread-lifetime-sanitized}"
cmake -S . -B "$out" -G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG' \
  -DCMAKE_CXX_FLAGS='-g1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
cmake --build "$out" --target hvm_thread_lifetime_tests -j 2
"$out/hvm_thread_lifetime_tests"
