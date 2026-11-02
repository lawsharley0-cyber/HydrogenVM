#!/usr/bin/env bash
# Windows/WSL and Linux entry point. Everything stays local; never pulls or resets Git.
set -euo pipefail
root=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)
mode=${1:-play}
client=${2:?Pass an IPA path (or an app/IPA folder for scan)}
out=${3:?Pass an output directory for logs and screenshots}
build=${HVM_LOCAL_BUILD:-/var/tmp/hvm-clash-build}
source_root=$root
state=${HVM_LOCAL_STATE:-/var/tmp/hvm-local}
python=/var/tmp/hvm-local-python/bin/python
mkdir -p "$out" "$state"
case "$mode" in play|app|check|scan|regress|trace|home) ;; *) echo 'Mode must be play, app, check, scan, regress, trace or home' >&2; exit 2 ;; esac
if [[ "$mode" == play || "$mode" == app || "$mode" == trace || "$mode" == home ]]; then
    exec 9>"$state/session.lock"
    flock -n 9 || { echo 'A local session is already using this sandbox. Close it first.' >&2; exit 2; }
fi
# WSL builds must not repeatedly traverse 9p for every source/header. Keep the
# editable checkout where it is and mirror only build inputs into ext4. A custom
# HVM_LOCAL_BUILD keeps its existing source/cache pairing; opt out explicitly
# with HVM_LOCAL_NATIVE_SOURCE=0 if a mounted-source build is wanted.
exec 8>"$state/build.lock"
flock 8
if [[ "$root" == /mnt/* && -z "${HVM_LOCAL_BUILD:-}" && "${HVM_LOCAL_NATIVE_SOURCE:-1}" != 0 ]]; then
    key=$(printf '%s' "$root" | sha256sum | cut -c1-12)
    source_root=/var/tmp/hvm-local/source-$key
    build=/var/tmp/hvm-local/build-$key
    if [[ -e "$source_root" && ! -f "$source_root/.hvm-source-mirror" ]]; then
        echo "Refusing to overwrite an unmarked source mirror: $source_root" >&2; exit 2
    fi
    mkdir -p "$source_root"
    if [[ "$(realpath "$source_root")" != "$source_root" || -L "$source_root/src" || -L "$source_root/tests" ]]; then
        echo 'Refusing a source mirror containing redirected paths' >&2; exit 2
    fi
    if [[ -f "$source_root/.hvm-source-mirror" && "$(cat "$source_root/.hvm-source-mirror")" != "$root" ]]; then
        echo 'Source mirror belongs to another checkout' >&2; exit 2
    fi
    printf '%s\n' "$root" > "$source_root/.hvm-source-mirror"
    # Content checks also prevent a Windows/WSL clock difference from hiding an
    # edited file behind a newer object timestamp. Changed inputs get Linux time.
    rsync -a --checksum --no-times --delete "$root/src/" "$source_root/src/"
    rsync -a --checksum --no-times --delete "$root/tests/" "$source_root/tests/"
    rsync -a --checksum --no-times "$root/CMakeLists.txt" "$source_root/"
    echo "Building from Linux storage: $source_root"
fi
cmake -S "$source_root" -B "$build" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release 2>&1 | tee "$out/configure.log"
cmake --build "$build" --target hvm hvm_tests hvm_machodump --parallel 4 2>&1 | tee "$out/build.log"
flock -u 8
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
    home)
        PYTHONDONTWRITEBYTECODE=1 python3 "$root/tools/clash_home_probe.py" --hvm "$build/hvm" --ipa "$client" \
            --out "$state/home-probe" --results "$out/offline-home" --seconds 90 --launches 2 --capture-audio
        ;;
    scan)
        python3 "$root/tools/compat.py" scan "$client" --hvm "$build/hvm" \
            --out "$out/library-scan-$(date +%Y%m%d-%H%M%S)" --seconds 15 --jobs 1 --relaunch
        ;;
    regress)
        # The three regression IPAs come from the private store (HVM_FIXTURE_ROOT, files under
        # ipas/), each checked against the SHA-256 in tools/compat_harness/manifests/apps.json.
        # Nothing is read from the repository or from the old HVM_LOCAL_FIXTURES directory.
        regress_ipas=$(python3 "$root/tools/resolve_fixtures.py" angry-birds tiny-tower art-of-glow) || {
            echo "Regression fixtures are unavailable. Set HVM_FIXTURE_ROOT to the private store (it holds ipas/<name>.ipa)." >&2
            exit 2
        }
        mapfile -t regress_ipa_list <<<"$regress_ipas"
        python3 "$root/tools/compat.py" scan "$client" "${regress_ipa_list[@]}" \
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
