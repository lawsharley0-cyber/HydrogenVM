# #29: Minecraft PE Lite world-load capture

A device fault needs its actual device evidence. tools/device_world_capture.py
checks capture metadata, artifact presence and SHA-256 integrity. It never
uploads files or decides compatibility: even a complete capture emits
compatibilityVerdict=unverified. Linux with 16 KB page emulation cannot satisfy
its iOS-host requirement. Tests first failed on absent validation, then cover
missing/modified files, escaping paths, wrong build/fixture hashes, malformed
scenario timestamps and required fault artifacts. A Boolean schema version
also failed before strict integer validation; all seven focused checks pass.

## Available private Linux observation

The available extracted minecraftpe.demo.app is bundle
com.mojang.minecraftpe-demo, version 0.1.2, executable SHA-256
6cf0d02b1eaa7d9ec84bf2e948cf32561a2d6134edbb3ca8ee5c15adf6610f35.
The original IPA is unavailable. This is not a verified match to historical
manifest IPA f91103af827e8ababce84554640fd71bc50a5c284cac7b12cd64c2008f95706d.

Main 4c09fa4708fe48796f3255695e97aef685087042, executable SHA-256
1b92fff9e5a9719e1088afac0e4428c360443a1752973c519a1da528e9ec39ad,
Linux/native pages 4096/software: a 60-second probe reached the visible
Generating world / Building terrain screen, presented 165 frames and executed
12,154,734,449 guest instructions. It reported no crash and completed graceful
saving. It did not establish completed world entry or reproduce the device
crash. Private commands, frame, screenshot, log and report remain under
HVM_FIXTURE_ROOT/evidence/minecraft-world-1793577224531844286.
No renderer/runtime workaround or fabricated crash evidence is included.

## Exact physical-iPhone capture

1. Install the candidate signed privately. Record its full source commit, build
   number and dirty status, device model/iOS, renderer, and native host page
   size (Xcode expression: (long)getpagesize()). Record original private IPA
   SHA-256, game version and bundle identifier. Preserve the existing guest
   home and record whether the world already existed.
2. Start screen recording before launch. Record elapsed times and exact taps
   for Play, world selection/creation, loading progress and the first failure.
   Include a screenshot at the loading state and, if reached, the first
   controllable world frame. Continue at least 60 seconds after confirmed
   entry; record actions and responsiveness. A splash or terrain-building
   frame alone is not a completed world-load result.
3. After a host crash, reopen HydrogenVM and export recovered #264 JSON with
   hostFaultDiagnostics plus the complete matching session log. If recovery
   fails, copy pending.json, pending.signal.json and pending.hostfault.txt from
   Documents/HydrogenVM/crashes before another launch. Export the matching
   iOS Analytics/Xcode .ips and dSYM, checking UUIDs with dwarfdump --uuid.
   Retain fault address/arena offset, guest PCs/LRs/HLE and all native stacks.
   For watchdog/jetsam, keep the OS .ips and describe that outcome accurately.
4. Store artifacts together privately. Make capture.json from the template
   below, replacing every placeholder with measured values and checksums.
   Host-crash requires session-log, screenshot, crash-json, ips and symbols;
   guest-crash requires crash-json; jetsam/watchdog requires ips. Do not invent
   a host-crash role for a run that did not crash. Verify locally:

   python3 tools/device_world_capture.py PRIVATE_CAPTURE/capture.json \
     --expected-commit FULL_INSTALLED_SHA --expected-ipa-sha256 PRIVATE_IPA_SHA

5. Give the owner private artifact references plus a scrubbed capture summary.
   Keep #29 open until the exact world-load failure is reproduced and diagnosed
   or the affected device/fixture completes the requested scenario.

## Unfilled metadata template

```json
{
  "schemaVersion": 1,
  "kind": "hvm-device-world-capture",
  "emulator": {"commit": "<full installed SHA>", "build": "<build>", "dirty": false},
  "host": {"system": "iOS", "deviceModel": "<model>", "osVersion": "<iOS>", "pageSize": 16384},
  "fixture": {"ipaSha256": "<private original IPA SHA256>", "bundleId": "<measured>", "version": "<measured>"},
  "scenario": {"durationSeconds": 0, "homeState": "<preserved home reference>",
    "outcome": "unconfirmed", "actions": [{"atSeconds": 0, "action": "<actual input>"}]},
  "artifacts": [{"role": "session-log", "path": "session.log", "sha256": "<actual SHA256>"},
    {"role": "screenshot", "path": "loading.png", "sha256": "<actual SHA256>"}]
}
```

The template is deliberately incomplete; replace pageSize and duration too.
It is not a successful capture. No private files belong in this repository.

Local unchanged-runtime gates: fulltest 487/487; Test Lab 25/25, zero skips; complete enabled tools suite 209/209, zero skips. The baseline runtime binary fingerprint is recorded above. This card changes evidence tooling only; thread/memory sanitizer work remains on #264/#263/#234/#28.
