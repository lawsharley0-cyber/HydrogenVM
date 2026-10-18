#!/usr/bin/env bash
# Builds the guest test binaries and the host-side expected results.
# Usage: tools/build_tests.sh <out-dir>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/tests}"
mkdir -p "$OUT"

CC=${CC_GUEST:-clang}
LINK="-fuse-ld=lld -nostdlib -static -Wl,-Ttext=0x10000 -Wl,--entry=0"
CFLAGS="-O2 -ffreestanding -fno-builtin -fno-stack-protector -mfloat-abi=soft -Wall -Wno-unused-function"

guest() {   # name, target flags...
  local name=$1; shift
  "$CC" --target=armv7-none-eabi $CFLAGS "$@" -c "$ROOT/tests/c/ctests.c" -o "$OUT/$name.ctests.o"
  "$CC" --target=armv7-none-eabi $CFLAGS "$@" -c "$ROOT/tests/c/rt64.c"   -o "$OUT/$name.rt64.o"
  "$CC" --target=armv7-none-eabi -mcpu=cortex-a8 -c "$ROOT/tests/c/rt.S"  -o "$OUT/$name.rt.o"
  "$CC" --target=armv7-none-eabi $LINK "$OUT/$name.ctests.o" "$OUT/$name.rt64.o" "$OUT/$name.rt.o" -o "$OUT/$name.elf"
}

# Guest builds of the C differential tests.
guest ctests_arm        -mcpu=cortex-a8 -marm
guest ctests_thumb      -mcpu=cortex-a8 -mthumb
guest ctests_thumb_idiv -mcpu=cortex-a15 -mthumb      # SDIV/UDIV (like armv7s / A6+)
guest ctests_arm_os     -mcpu=cortex-a8 -marm -Os
guest ctests_thumb_os   -mcpu=cortex-a8 -mthumb -Os

# Host build → expected results.
NAMES=$(grep -oE '^TEST\(t_[a-z0-9_]+\)' "$ROOT/tests/c/ctests.c" | sed -E 's/TEST\((.*)\)/\1/')
{
  echo '#include <stdint.h>'
  echo '#include <stdio.h>'
  for n in $NAMES; do echo "uint64_t $n(void);"; done
  echo 'int main(void) {'
  for n in $NAMES; do echo "  printf(\"$n %llx\\n\", (unsigned long long)$n());"; done
  echo '  return 0; }'
} > "$OUT/host_main.c"
clang -O1 -fno-builtin -Wno-unused-function "$ROOT/tests/c/ctests.c" "$OUT/host_main.c" -o "$OUT/host_expected"
"$OUT/host_expected" > "$OUT/expected.txt"

# Hand-written unit tests.
for f in "$ROOT"/tests/asm/*.S; do
  b=$(basename "$f" .S)
  "$CC" --target=armv7-none-eabi -mcpu=cortex-a15 -c "$f" -o "$OUT/$b.o"
  "$CC" --target=armv7-none-eabi $LINK "$OUT/$b.o" -o "$OUT/$b.elf"
done
echo "built test binaries in $OUT"
