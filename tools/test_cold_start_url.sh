#!/usr/bin/env bash
# Exercise a real cold-start URL with the simulator-only COLD_START_URL_TEST build.
# Usage: tools/test_cold_start_url.sh <HydrogenVM.app> <FoundationTest.app>
set -euo pipefail

HOST_APP=$1
GUEST_APP=$2
BUNDLE_ID=com.lfull.HydrogenVM
UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
devices = json.load(sys.stdin)["devices"]
for runtime in sorted(devices, reverse=True):
    if "iOS" not in runtime: continue
    for device in devices[runtime]:
        if device["name"].startswith("iPhone"):
            print(device["udid"]); sys.exit(0)
sys.exit("No iPhone simulator")')
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null
xcrun simctl uninstall "$UDID" "$BUNDLE_ID" 2>/dev/null || true
xcrun simctl install "$UDID" "$HOST_APP"
trap 'xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true' EXIT

DATA=$(xcrun simctl get_app_container "$UDID" "$BUNDLE_ID" data)
GUEST_ID=$(python3 -c 'import plistlib, sys; print(plistlib.load(open(sys.argv[1], "rb"))["CFBundleIdentifier"])' "$GUEST_APP/Info.plist")
xcrun simctl terminate "$UDID" "$BUNDLE_ID" 2>/dev/null || true
rm -f "$DATA/Documents/cold-start-queued" "$DATA/Documents/cold-start-routed" "$DATA/Documents/cold-start-guest-passed"
mkdir -p "$DATA/Documents/Apps/$GUEST_ID"
cp -R "$GUEST_APP" "$DATA/Documents/Apps/$GUEST_ID/"
URL="hydrogenvm://run/$GUEST_ID"

# The app is terminated. No autorun hook is set: the launch URL has to reach the app's
# SwiftUI `.onOpenURL` handler, which queues it (cold-start-queued) before the library
# resolves it (cold-start-routed). A link the app never queues cannot be hidden by a
# later delivery, and nothing else in the test build opens a game.
xcrun simctl openurl "$UDID" "$URL"
for _ in $(seq 1 240); do
    [ -f "$DATA/Documents/cold-start-guest-passed" ] && break
    sleep 0.5
done

# A failed stage is annotated as well as printed: workflow logs need admin rights to
# download, annotations are readable from the run page.
app_log() {
    xcrun simctl spawn "$UDID" log show --last 5m --style compact \
        --predicate 'process == "HydrogenVM"' 2>/dev/null | tail -n 20 | tr '\n' ' ' | cut -c1-600
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
xcrun simctl openurl "$UDID" "$MALFORMED_URL"
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
