#!/usr/bin/env bash
# Nightly entry point for WSL (or any Linux box). Keeps its own clone of the repository, moves it
# to the newest main, builds hvm and runs every game in the fixtures folder. Your own checkout is
# never touched.
#
#   tools/nightly/run_nightly.sh /mnt/c/Users/<you>/Desktop/test_ipa [--publish]
#
# Environment: HVM_NIGHTLY_DIR (default /var/tmp/hvm-nightly), HVM_NIGHTLY_REPO (clone URL),
#              HVM_NIGHTLY_SECONDS (per game, default 45).
set -euo pipefail
fixtures=${1:?Pass the folder that holds the .ipa and .apk files}
shift
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

exec python3 "$src/tools/nightly/nightly.py" --fixtures "$fixtures" --hvm "$build/hvm" \
    --state "$dir/state" --seconds "${HVM_NIGHTLY_SECONDS:-45}" "$@"
