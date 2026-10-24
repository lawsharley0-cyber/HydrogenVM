#!/usr/bin/env bash
# Run a guest test app inside the HydrogenVM iPhone app in the iOS Simulator
# (macOS + Xcode only). Used by CI; also works locally.
#   tools/sim_test.sh <HydrogenVM.app (simulator build)> <guest .app> <expected line> [taps] [out-dir]
# taps: "x,y@t;x,y@t" — seconds after the guest's first frame.
set -uo pipefail
HOST_APP=$1 GUEST_APP=$2 WANT=$3 TAPS=${4:-} OUTDIR=${5:-sim-out}
mkdir -p "$OUTDIR"
LOG="$OUTDIR/console.log"

# Newest available iPhone simulator.
UDID=$(xcrun simctl list devices available -j | python3 -c '
import json, sys
d = json.load(sys.stdin)["devices"]
rts = sorted((r for r in d if "iOS" in r), key=lambda r: [int(x) for x in r.split("iOS-")[-1].split("-")])
for r in reversed(rts):
    for dev in d[r]:
        if dev["name"].startswith("iPhone"):
            print(dev["udid"]); sys.exit()
')
[ -n "$UDID" ] || { echo "no iPhone simulator available"; xcrun simctl list devices available; exit 1; }
echo "simulator: $UDID"
xcrun simctl boot "$UDID" 2>/dev/null || true
xcrun simctl bootstatus "$UDID" -b >/dev/null

xcrun simctl uninstall "$UDID" com.lfull.HydrogenVM 2>/dev/null || true
xcrun simctl install "$UDID" "$HOST_APP"
DATA=$(xcrun simctl get_app_container "$UDID" com.lfull.HydrogenVM data)
GUEST_ID=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$GUEST_APP/Info.plist")
mkdir -p "$DATA/Documents/Apps/$GUEST_ID"
cp -R "$GUEST_APP" "$DATA/Documents/Apps/$GUEST_ID/"
echo "installed guest $GUEST_ID"

SIMCTL_CHILD_HVM_AUTORUN="$GUEST_ID" SIMCTL_CHILD_HVM_AUTOTAP="$TAPS" \
  xcrun simctl launch --console-pty --terminate-running-process "$UDID" com.lfull.HydrogenVM >"$LOG" 2>&1 &
LAUNCH=$!

# Wait for the guest to exit (or 120 s); grab a screenshot once it is drawing.
shot=0
for _ in $(seq 1 240); do
  sleep 0.5
  if [ $shot = 0 ] && grep -q '\[autotap\]' "$LOG" 2>/dev/null; then
    sleep 0.2; xcrun simctl io "$UDID" screenshot "$OUTDIR/screen.png" >/dev/null 2>&1 && shot=1
  fi
  if grep -qE '\[(hvm-exit|hvm-batch-exit)\]' "$LOG" 2>/dev/null; then break; fi
done
[ $shot = 1 ] || xcrun simctl io "$UDID" screenshot "$OUTDIR/screen.png" >/dev/null 2>&1
sleep 1
kill $LAUNCH 2>/dev/null || true
xcrun simctl terminate "$UDID" com.lfull.HydrogenVM 2>/dev/null || true
# Host-side crash logs, if the emulator itself crashed.
cp ~/Library/Logs/DiagnosticReports/HydrogenVM* "$OUTDIR/" 2>/dev/null || true
if [ -d "$DATA/Documents/TestReports" ]; then
  cp -R "$DATA/Documents/TestReports/." "$OUTDIR/" 2>/dev/null || true
fi

echo "------ console (last 60 lines) ------"
tail -60 "$LOG"
echo "-------------------------------------"
if grep -qxF "$WANT" <(tr -d '\r' <"$LOG"); then echo "PASS: found '$WANT'"; exit 0; fi
echo "FAIL: '$WANT' not in console output"
exit 1
