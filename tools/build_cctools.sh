#!/usr/bin/env bash
# Build Apple's ld64 for Linux (cctools-port + apple-libdispatch), used by
# tools/ios_build.py to link the 32-bit iOS test apps.
#   tools/build_cctools.sh [prefix]     (default: <repo-root>/tools/toolchain)
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
PREFIX="${1:-$REPO_ROOT/tools/toolchain}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
export CC="${CC:-clang}" CXX="${CXX:-clang++}"

if [ -d "$PREFIX/bin" ]; then
    export PATH="$PREFIX/bin:$PATH"
fi

git clone -q --depth 1 https://github.com/tpoechtrager/apple-libdispatch "$WORK/libdispatch"
cmake -S "$WORK/libdispatch" -B "$WORK/libdispatch/build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$PREFIX" >/dev/null
ninja -C "$WORK/libdispatch/build" install >/dev/null

git clone -q --depth 1 https://github.com/tpoechtrager/cctools-port "$WORK/cctools-port"
cd "$WORK/cctools-port/cctools"
./configure --prefix="$PREFIX" --target=arm-apple-darwin11 \
            --with-libdispatch="$PREFIX" --with-libblocksruntime="$PREFIX" >/dev/null
make -j"$(nproc)" >/dev/null
make install >/dev/null
echo "ld64 installed: $PREFIX/bin/arm-apple-darwin11-ld"
