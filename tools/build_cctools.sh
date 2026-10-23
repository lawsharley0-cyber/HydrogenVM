#!/usr/bin/env bash
# Build Apple's ld64 for Linux (cctools-port + apple-libdispatch), used by
# tools/ios_build.py to link the 32-bit iOS test apps.
#   tools/build_cctools.sh [prefix]     (default /opt/cctools)
set -euo pipefail
ulimit -s 65536 2>/dev/null || true
PREFIX="${1:-/opt/cctools}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
git clone -q --depth 1 https://github.com/tpoechtrager/apple-libdispatch "$WORK/libdispatch"
CC=clang CXX=clang++ cmake -S "$WORK/libdispatch" -B "$WORK/libdispatch/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
ninja -C "$WORK/libdispatch/build" install >/dev/null

git clone -q --depth 1 https://github.com/tpoechtrager/cctools-port "$WORK/cctools-port"
cd "$WORK/cctools-port/cctools"
./configure --prefix="$PREFIX" --target=arm-apple-darwin11 \
            --with-libdispatch="$PREFIX" --with-libblocksruntime="$PREFIX" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "ld64 installed: $PREFIX/bin/arm-apple-darwin11-ld"
