#!/usr/bin/env bash
# Build the 32-bit iOS test apps (clang + Apple ld64 against the stub SDK) and
# run them in HydrogenVM, checking their results.
#   tools/ios_tests.sh [build-dir]
# HVM_PREBUILT=1 skips building and runs the apps already in <build-dir>/ios
# (CI builds them on Linux, where ld64 lives, and runs them on macOS too).
set -uo pipefail
command -v timeout >/dev/null || timeout() { shift; "$@"; }
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
B="${1:-$ROOT/build}"
OUT="$B/ios"
APPS="$ROOT/tests/ios/apps"
HVM="$B/hvm"
BUILD="python3 $ROOT/tools/ios_build.py app --out $OUT"
mkdir -p "$OUT"
fail=0
step() { printf '%-28s' "$1"; }

if [ "${HVM_PREBUILT:-0}" != 1 ]; then
step "build CTest";           $BUILD --name CTest "$APPS/ctest/main.c" >/dev/null && echo ok || fail=1
step "build CTestLegacy";     $BUILD --name CTestLegacy --min-ios 3.0 --no-pie "$APPS/ctest/main.c" >/dev/null && echo ok || fail=1
step "build BinPlist";        $BUILD --name BinPlist --binary-plist "$APPS/ctest/main.c" >/dev/null && echo ok || fail=1
step "build ObjCTest";        $BUILD --name ObjCTest "$APPS/objctest/main.m" >/dev/null && echo ok || fail=1
step "build FoundationTest";  (cd "$APPS/foundationtest" && $BUILD --name FoundationTest --resource config.plist --resource en.lproj main.m >/dev/null) && echo ok || fail=1
RES="$OUT/_work/uikit-res"
python3 "$APPS/uikittest/make_resources.py" "$RES"
step "build UIKitTest";       $BUILD --name UIKitTest --resource "$RES/test.png" --resource "$RES/test@2x.png" --resource "$RES/crushed.png" \
                                --resource "$RES/Panel.nib" --resource "$RES/PanelKeyed.nib" "$APPS/uikittest/main.m" >/dev/null && echo ok || fail=1
fi

run() {   # name, expected exit code, expected last line, extra args...
  local name=$1 code=$2 want=$3; shift 3
  local home; home=$(mktemp -d)
  step "run $name"
  local out rc
  out=$(timeout 120 "$HVM" run "$OUT/$name.app" --home "$home" --quiet "$@" 2>&1); rc=$?
  rm -rf "$home"
  if [ "$rc" = "$code" ] && printf '%s\n' "$out" | grep -qxF "$want"; then echo ok
  else echo "FAILED (exit $rc)"; printf '%s\n' "$out" | grep -v '^ok ' | tail -25; fail=1; fi
}
run CTest 3 "atexit handler ran"
run CTestLegacy 3 "atexit handler ran"
run BinPlist 3 "atexit handler ran"
run ObjCTest 0 "ALL CHECKS PASSED"
run FoundationTest 0 "ALL CHECKS PASSED"
run UIKitTest 0 "ALL CHECKS PASSED" --tap 70,350@0.3 --tap 230,350@0.5 --screenshot "$OUT/UIKitTest.png"
exit $fail
