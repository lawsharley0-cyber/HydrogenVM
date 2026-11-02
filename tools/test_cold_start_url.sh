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

# The app is terminated. No autorun hook is set:
# only the scene delegate can queue this URL in the test build; the SwiftUI
# onOpenURL handler is disabled so a later delivery cannot hide a lost handoff.
xcrun simctl openurl "$UDID" "$URL"
for _ in $(seq 1 240); do
    [ -f "$DATA/Documents/cold-start-guest-passed" ] && break
    sleep 0.5
done

for stage in queued routed guest-passed; do
    file="$DATA/Documents/cold-start-$stage"
    [ -f "$file" ] || { echo "FAIL: missing $stage marker"; exit 1; }
    expected=$GUEST_ID
    [ "$stage" != queued ] || expected=$URL
    [ "$(cat "$file")" = "$expected" ] || { echo "FAIL: unexpected $stage marker"; exit 1; }
done
echo "PASS: scene queued $URL, library routed $GUEST_ID, and the guest passed"
