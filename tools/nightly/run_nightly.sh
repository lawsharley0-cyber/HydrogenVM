#!/usr/bin/env bash
# Nightly entry point for WSL (or any Linux box). Keeps its own clone of the repository, moves it
# to the newest main, builds hvm and runs every game in the fixtures folder. Your own checkout is
# never touched.
#
#   tools/nightly/run_nightly.sh /mnt/c/Users/<you>/Desktop/IPA /mnt/c/Users/<you>/Desktop/APK [--publish]
#   tools/nightly/run_nightly.sh --fixtures /mnt/c/Users/<you>/Desktop/IPA --fixtures /mnt/c/Users/<you>/Desktop/APK [--publish]
#
# Environment: HVM_NIGHTLY_DIR (default /var/tmp/hvm-nightly), HVM_NIGHTLY_REPO (clone URL),
#              HVM_NIGHTLY_SECONDS (per game, default 45).
set -euo pipefail

fixture_args=()
extra_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fixtures)
            [[ $# -ge 2 ]] || { echo "Missing directory after --fixtures" >&2; exit 2; }
            fixture_args+=(--fixtures "$2")
            shift 2
            ;;
        --*)
            extra_args+=("$1")
            shift
            if [[ $# -gt 0 && "$1" != --* && ( "${extra_args[-1]}" == "--only" || "${extra_args[-1]}" == "--repo" || "${extra_args[-1]}" == "--seconds" || "${extra_args[-1]}" == "--state" || "${extra_args[-1]}" == "--hvm" ) ]]; then
                extra_args+=("$1")
                shift
            fi
            ;;
        *)
            if [[ ${#extra_args[@]} -eq 0 ]]; then
                fixture_args+=(--fixtures "$1")
            else
                extra_args+=("$1")
            fi
            shift
            ;;
    esac
done
[[ ${#fixture_args[@]} -gt 0 ]] || { echo "Pass at least one fixtures folder" >&2; exit 2; }

dir=${HVM_NIGHTLY_DIR:-/var/tmp/hvm-nightly}
repo=${HVM_NIGHTLY_REPO:-https://github.com/lawsharley0-cyber/HydrogenVM.git}
src=$dir/src
build=$dir/build
mkdir -p "$dir"
exec 9>"$dir/nightly.lock"
flock -n 9 || { echo 'A nightly run is already going.' >&2; exit 2; }

if [[ ! -d "$src/.git" ]]; then
    git clone --quiet "$repo" "$src"
fi
git -C "$src" fetch --quiet origin main
git -C "$src" checkout --quiet --detach origin/main      # this clone belongs to the nightly run only
echo "[nightly] building $(git -C "$src" rev-parse --short HEAD)"
cmake -S "$src" -B "$build" -G Ninja -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=Release > "$dir/configure.log" 2>&1
cmake --build "$build" --target hvm --parallel "$(nproc)" > "$dir/build.log" 2>&1 || {
    echo "[nightly] build failed; see $dir/build.log" >&2; exit 2; }

exec python3 "$src/tools/nightly/nightly.py" "${fixture_args[@]}" --hvm "$build/hvm" \
    --state "$dir/state" --seconds "${HVM_NIGHTLY_SECONDS:-45}" "${extra_args[@]}"
