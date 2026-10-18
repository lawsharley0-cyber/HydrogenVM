#!/usr/bin/env bash
# Build everything and run the full test suite.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="$ROOT/build"
cmake -S "$ROOT" -B "$B" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release >/dev/null || exit 1
ninja -C "$B" >/dev/null || { ninja -C "$B"; exit 1; }
"$ROOT/tools/build_tests.sh" "$B/tests" >/dev/null || exit 1
rc=0
"$B/hvm_tests" selftest || rc=1
for f in "$ROOT"/tests/asm/*.S; do
  b=$(basename "$f" .S)
  "$B/hvm_tests" asm "$B/tests/$b.elf" "$f" || rc=1
done
for e in ctests_arm ctests_arm_os ctests_thumb ctests_thumb_os ctests_thumb_idiv; do
  "$B/hvm_tests" c "$B/tests/$e.elf" "$B/tests/expected.txt" || rc=1
done
exit $rc
