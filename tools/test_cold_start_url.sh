#!/usr/bin/env bash
# Exercise a real cold-start URL with the simulator-only COLD_START_URL_TEST build.
# Usage: tools/test_cold_start_url.sh <HydrogenVM.app> <FoundationTest.app>
# CI supplies HVM_COLD_START_XCTESTRUN to accept the system Open confirmation.
set -euo pipefail

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
HOST_APP=$1
GUEST_APP=$2
BUNDLE_ID=com.lfull.HydrogenVM
UDID=${HVM_COLD_START_UDID:-}
if [ -z "$UDID" ]; then
UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
devices = json.load(sys.stdin)["devices"]
for runtime in sorted(devices, reverse=True):
    if "iOS" not in runtime: continue
    for device in devices[runtime]:
        if device["name"].startswith("iPhone"):
            print(device["udid"]); sys.exit(0)
sys.exit("No iPhone simulator")')
fi
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl uninstall "$UDID" "$BUNDLE_ID" 2>/dev/null || true
xcrun simctl install "$UDID" "$HOST_APP"
trap 'xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true' EXIT

# Initialize installation/container state, then terminate the process before
# the actual URL launch. A Home Screen icon is created from an already-used app.
SETUP_LAUNCH=$(xcrun simctl launch "$UDID" "$BUNDLE_ID")
echo "$SETUP_LAUNCH"
SETUP_PID=${SETUP_LAUNCH##*: }
case "$SETUP_PID" in
    ''|*[!0-9]*) echo "::error::Cannot identify setup process: $SETUP_LAUNCH"; exit 1 ;;
esac
DATA=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)
for _ in $(seq 1 40); do
    DATA=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)
    [ -f "$DATA/Documents/cold-start-launched" ] && break
    sleep 0.5
done
[ -f "$DATA/Documents/cold-start-launched" ] || { echo "::error::Instrumented setup launch did not write its marker"; exit 1; }
xcrun simctl terminate "$UDID" "$BUNDLE_ID"
# Simulator app processes are host processes. Do not open the URL while the
# terminated setup instance can still receive it instead of a fresh launch.
for _ in $(seq 1 40); do
    kill -0 "$SETUP_PID" 2>/dev/null || break
    sleep 0.5
done
if kill -0 "$SETUP_PID" 2>/dev/null; then
    echo "::error::Setup process $SETUP_PID did not exit after termination"
    exit 1
fi
echo "PASS: setup process $SETUP_PID exited before the cold URL"

GUEST_ID=$(python3 -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))["CFBundleIdentifier"])' "$GUEST_APP/Info.plist")
rm -f "$DATA/Documents/cold-start-launched" "$DATA/Documents/cold-start-error" "$DATA/Documents/cold-start-queued" "$DATA/Documents/cold-start-routed" "$DATA/Documents/cold-start-guest-passed"
mkdir -p "$DATA/Documents/Apps/$GUEST_ID"
cp -R "$GUEST_APP" "$DATA/Documents/Apps/$GUEST_ID/"
URL="hydrogenvm://run/$GUEST_ID"

# The app is terminated. No autorun hook is set: the launch URL has to reach the app's
# SwiftUI `.onOpenURL` handler, which queues it (cold-start-queued) before the library
# resolves it (cold-start-routed). A link the app never queues cannot be hidden by a
# later delivery, and nothing else in the test build opens a game.
# A failed stage is annotated as well as printed: workflow logs need admin rights to
# download, annotations are readable from the run page.
app_log() {
    local out=${HVM_COLD_START_OUT:-build/cold-start-diagnostics}
    mkdir -p "$out"
    xcrun simctl io "$UDID" screenshot "$out/screen.png" >"$out/screenshot.log" 2>&1 || true
    xcrun simctl spawn "$UDID" log show --last 5m --style compact \
        --predicate 'process == "HydrogenVM" OR process == "SpringBoard" OR process == "runningboardd"' \
        >"$out/system.log" 2>&1 || true
    xcrun simctl spawn "$UDID" launchctl list >"$out/launchctl.log" 2>&1 || true
    xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data >"$out/container.txt" 2>&1 || true
    cp "$DATA"/Documents/cold-start-* "$out/" 2>/dev/null || true
    tail -n 80 "$out/system.log"
}

fail() {
    echo "::error title=cold-start URL handoff::$*"
    echo "::error title=cold-start app log::See the app log printed below."
    app_log
    echo "FAIL: $*"
    echo "--- ${DATA}/Documents ---"
    ls -la "$DATA/Documents" 2>/dev/null || true
    exit 1
}

COLD_URL_COUNT=0
open_url() {
    COLD_URL_COUNT=$((COLD_URL_COUNT + 1))
    xcrun simctl openurl "$UDID" "$1"
    if [ -n "${HVM_COLD_START_XCTESTRUN:-}" ]; then
        local out=${HVM_COLD_START_OUT:-build/cold-start-diagnostics}
        mkdir -p "$out"
        # One retry: on hosted runners the system Open alert sometimes stays up
        # after the helper's tap (run 42580285720). The URL is still pending, so
        # a second helper run accepts the same alert. Marker checks are unchanged.
        local attempt log
        for attempt in 1 2; do
            log="$out/confirmation-$COLD_URL_COUNT-$attempt.log"
            if xcodebuild -xctestrun "$HVM_COLD_START_XCTESTRUN" \
                -destination "platform=iOS Simulator,id=$UDID" -parallel-testing-enabled NO \
                -resultBundlePath "$out/confirmation-$COLD_URL_COUNT-$attempt.xcresult" \
                test-without-building >"$log" 2>&1; then
                return 0
            fi
            tail -100 "$log"
            bash "$SCRIPT_DIR/annotate_xctest_failures.sh" "$log" "cold-start confirmation, attempt $attempt"
            if [ "$attempt" = 1 ]; then echo "Retrying the system URL confirmation once"; fi
        done
        fail "system URL confirmation did not complete"
    fi
}

open_url "$URL"
for _ in $(seq 1 240); do
    [ -f "$DATA/Documents/cold-start-guest-passed" ] && break
    sleep 0.5
done

for stage in launched queued routed guest-passed; do
    file="$DATA/Documents/cold-start-$stage"
    [ -f "$file" ] || fail "missing $stage marker"
    expected=$GUEST_ID
    case "$stage" in
        launched) expected=$BUNDLE_ID ;;   # the process ran at all
        queued) expected=$URL ;;           # the link reached the app and waits
    esac
    [ "$(cat "$file")" = "$expected" ] || fail "unexpected $stage marker: $(cat "$file")"
done
echo "PASS: app launched, the link queued $URL, the library routed $GUEST_ID, and the guest passed"

# A malformed URL in our scheme must still reach the scene router, so its
# resolver can present the malformed-link error instead of silently dropping it.
rm -f "$DATA/Documents/cold-start-error"
MALFORMED_URL="$URL?source=clip"
open_url "$MALFORMED_URL"
for _ in $(seq 1 40); do
    [ "$(cat "$DATA/Documents/cold-start-queued")" = "$MALFORMED_URL" ] && break
    sleep 0.5
done
[ "$(cat "$DATA/Documents/cold-start-queued")" = "$MALFORMED_URL" ] || fail "malformed URL was dropped before routing"
echo "PASS: malformed HydrogenVM URL reached the scene router"

for _ in $(seq 1 40); do
    [ -f "$DATA/Documents/cold-start-error" ] && break
    sleep 0.5
done
[ -f "$DATA/Documents/cold-start-error" ] || fail "malformed URL did not reach the error handler"
[ "$(cat "$DATA/Documents/cold-start-error")" = "That link is not a HydrogenVM game link. A game icon uses hydrogenvm://run/<game-id>." ] || fail "unexpected malformed-link error"
echo "PASS: malformed URL resolved to the library error message"
