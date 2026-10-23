#!/usr/bin/env bash
# Builds the guest test binaries and the host-side expected results.
# Usage: tools/build_tests.sh <out-dir>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${1:-$ROOT/build/tests}"
mkdir -p "$OUT"

CC=${CC_GUEST:-clang}
if command -v lld >/dev/null 2>&1 || command -v ld.lld >/dev/null 2>&1; then
  LINK="-fuse-ld=lld -nostdlib -static -Wl,-Ttext=0x10000 -Wl,--entry=0"
else
  LINK="-nostdlib -static -Wl,-Ttext=0x10000 -Wl,--entry=0"
fi
BASE="-O2 -ffreestanding -fno-builtin -fno-stack-protector -fno-math-errno -ffp-contract=off -Wall -Wno-unused-function"
C="$ROOT/tests/c"

obj() {   # out-object, source, flags...
  local o=$1 src=$2; shift 2
  "$CC" --target=armv7-none-eabi $BASE "$@" -c "$src" -o "$o"
}

# guest <elf-name> <float-abi flags (also used for rt64)> -- <sources and per-source flags>
# Each guest build links the runtime (rt.S always ARM; rt64.c with the build's flags).
link_guest() {
  local name=$1; shift
  "$CC" --target=armv7-none-eabi $LINK "$@" -o "$OUT/$name.elf"
}

# ---------------------------------------------------- integer suite (ctests)
int_guest() {
  local name=$1; shift
  obj "$OUT/$name.ctests.o" "$C/ctests.c" -mfloat-abi=soft "$@"
  obj "$OUT/$name.rt64.o"   "$C/rt64.c"   -mfloat-abi=soft "$@"
  "$CC" --target=armv7-none-eabi -mcpu=cortex-a8 -c "$C/rt.S" -o "$OUT/$name.rt.o"
  link_guest "$name" "$OUT/$name.ctests.o" "$OUT/$name.rt64.o" "$OUT/$name.rt.o"
}
int_guest ctests_arm        -mcpu=cortex-a8 -marm
int_guest ctests_thumb      -mcpu=cortex-a8 -mthumb
int_guest ctests_thumb_idiv -mcpu=cortex-a15 -mthumb      # SDIV/UDIV (like armv7s / A6+)
int_guest ctests_arm_os     -mcpu=cortex-a8 -marm -Os
int_guest ctests_thumb_os   -mcpu=cortex-a8 -mthumb -Os

# ------------------------------------------------- float / SIMD suite (ftests)
# vfast.c gets -ffast-math only where NEON is enabled (so clang uses NEON for floats).
fp_guest() {
  local name=$1 fast=$2; shift 2
  obj "$OUT/$name.ftests.o" "$C/ftests.c" "$@"
  obj "$OUT/$name.vfast.o"  "$C/vfast.c" $fast "$@"
  obj "$OUT/$name.rt64.o"   "$C/rt64.c" "$@"
  "$CC" --target=armv7-none-eabi -mcpu=cortex-a8 -c "$C/rt.S" -o "$OUT/$name.rt.o"
  link_guest "$name" "$OUT/$name.ftests.o" "$OUT/$name.vfast.o" "$OUT/$name.rt64.o" "$OUT/$name.rt.o"
}
fp_guest ftests_vfp_arm_hard     ""            -mcpu=cortex-a8 -marm   -mfpu=vfpv3 -mfloat-abi=hard
fp_guest ftests_vfp_thumb_softfp ""            -mcpu=cortex-a8 -mthumb -mfpu=vfpv3-d16 -mfloat-abi=softfp
fp_guest ftests_neon_thumb       "-ffast-math" -mcpu=cortex-a8 -mthumb -mfpu=neon -mfloat-abi=softfp -O3
fp_guest ftests_neon_arm         "-ffast-math" -mcpu=cortex-a8 -marm   -mfpu=neon -mfloat-abi=hard -O3
fp_guest ftests_neon_thumb_os    "-ffast-math" -mcpu=cortex-a9 -mthumb -mfpu=neon -mfloat-abi=softfp -Os

# ------------------------------------------------------ host expected values
host_expected() {   # name, sources...
  local name=$1; shift
  local names
  names=$(grep -ohE '^TEST\([a-z0-9_]+\)' "$@" | sed -E 's/TEST\((.*)\)/\1/')
  {
    echo '#include <stdint.h>'
    echo '#include <stdio.h>'
    for n in $names; do echo "uint64_t $n(void);"; done
    echo 'int main(void) {'
    for n in $names; do echo "  printf(\"$n %llx\\n\", (unsigned long long)$n());"; done
    echo '  return 0; }'
  } > "$OUT/host_$name.c"
  clang -O1 -fno-builtin -fno-math-errno -ffp-contract=off -Wno-unused-function "$@" "$OUT/host_$name.c" -o "$OUT/host_$name" -lm
  "$OUT/host_$name" > "$OUT/expected_$name.txt"
}
host_expected ctests "$C/ctests.c"
host_expected ftests "$C/ftests.c" "$C/vfast.c"
cp "$OUT/expected_ctests.txt" "$OUT/expected.txt"

# ------------------------------------------------------- hand-written tests
for f in "$ROOT"/tests/asm/*.S; do
  b=$(basename "$f" .S)
  "$CC" --target=armv7-none-eabi -mcpu=cortex-a15 -mfpu=neon-vfpv4 -c "$f" -o "$OUT/$b.o"
  "$CC" --target=armv7-none-eabi $LINK "$OUT/$b.o" -o "$OUT/$b.elf"
done
echo "built test binaries in $OUT"
