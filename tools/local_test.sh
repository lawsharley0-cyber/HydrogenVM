#!/usr/bin/env bash
# Windows/WSL and Linux entry point. Everything stays local; never pulls or resets Git.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-play}
client=${2:?Pass an IPA path (or an app/IPA folder for scan)}
out=${3:?Pass an output directory for logs and screenshots}
build=${HVM_LOCAL_BUILD:-/var/tmp/hvm-clash-build}
state=${HVM_LOCAL_STATE:-/var/tmp/hvm-local}
python=/var/tmp/hvm-local-python/bin/python
mkdir -p "$out" "$state"
case "$mode" in play|app|check|scan|regress|trace) ;; *) echo 'Mode must be play, app, check, scan, regress or trace' >&2; exit 2 ;; esac
if [[ "$mode" == play || "$mode" == app || "$mode" == trace ]]; then
    exec 9>"$state/session.lock"
    flock -n 9 || { echo 'A local session is already using this sandbox. Close it first.' >&2; exit 2; }
fi
cmake -S "$root" -B "$build" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$out/configure.log"
cmake --build "$build" --target hvm hvm_tests hvm_machodump --parallel 4 2>&1 | tee "$out/build.log"
case "$mode" in
    play|app)
        if [[ ! -x "$python" ]]; then python3 -m venv /var/tmp/hvm-local-python; fi
        if ! "$python" -c 'import pygame' >/dev/null 2>&1; then "$python" -m pip install pygame==2.6.1; fi
        if [[ "$mode" == play ]]; then
            "$python" "$root/tools/desktop.py" --hvm "$build/hvm" --ipa "$client" --home "$state/home" --out "$out" --pinned-clash
        else
            "$python" "$root/tools/desktop.py" --hvm "$build/hvm" --ipa "$client" --home "$state/apps" --out "$out" --versioned-home
        fi
        ;;
    check) "$build/hvm_tests" fulltest 2>&1 | tee "$out/runtime-checks.txt" ;;
    scan)
        python3 "$root/tools/compat.py" scan "$client" --hvm "$build/hvm" \
            --out "$out/library-scan-$(date +%Y%m%d-%H%M%S)" --seconds 15 --jobs 1 --relaunch
        ;;
    regress)
        fixtures=${HVM_LOCAL_FIXTURES:-/var/tmp/hvm-fixtures}
        for fixture in AngryBirds_v1.0_os30.ipa Tiny_Tower_v1.0_os30.ipa Art_Of_Glow_v1.1.2_os313.ipa; do
            if [[ ! -f "$fixtures/$fixture" ]]; then echo "Missing regression fixture: $fixtures/$fixture" >&2; exit 2; fi
        done
        python3 "$root/tools/compat.py" scan "$client" "$fixtures/AngryBirds_v1.0_os30.ipa" "$fixtures/Tiny_Tower_v1.0_os30.ipa" "$fixtures/Art_Of_Glow_v1.1.2_os313.ipa" \
            --hvm "$build/hvm" --out "$out/regression-$(date +%Y%m%d-%H%M%S)" --seconds 15 --jobs 1 --relaunch
        ;;
    trace)
        # Diagnostic imports use a separate sandbox; never overwrite the live
        # desktop's container through hvm's replace-on-import IPA command.
        HVM_DUMP_SHADERS="$out/shaders" "$build/hvm" run "$client" --home "$state/trace-home-$(date +%Y%m%d-%H%M%S)" --run-for 30 --graceful \
            --screenshot "$out/trace.png" --report "$out/trace.json" --trace >"$out/trace.log" 2>&1
        echo "Trace saved in $out/trace.log"
        ;;
esac
