# Clash of Clans 1.70 — Linux bench, 13 November 2026

Blocked: the game needs Supercell's 2012 game server. A shared Security.framework
fix is included so this and other apps get past `SecCertificateCreateWithData`.

## IPA

- Source: [Legacy Store app 529479190](https://legacystore.app/app/529479190),
  version 1.70, the 29.8 MB copy not marked Encrypted:
  `https://archive.org/download/clash-of-clans-1.70.ipa/Clash_of_Clans_1.70.ipa`
- SHA-256: `f1186092ef0211872d8a1df74166453031d2c895a766d99291cc8610847d9fd0`
- Unpacked at `~/bench/apps/Clash_of_Clans_1.70` (not in the repo)
- Bundle `com.supercell.magic`, minimum iOS 4.3, `UIDeviceFamily` [1, 2],
  landscape only, armv7 thin, `cryptid` 0

No existing PR or branch already covered this IPA. Work is on current main
(`6917fc2`, including #445) in `codex/clash-of-clans-compat`.

## First launch (unfixed main)

```sh
H=$(mktemp -d)
./build/hvm run ~/bench/apps/Clash_of_Clans_1.70/Payload/Clash\ of\ Clans.app \
  --home "$H" --keep-going --run-for 60 --gpu --graceful \
  --report r.json --screenshot s.png
```

Crashed in 0.23 s with no frame presented:

```
Stopped: _CFArrayCreate: *** -[NSArray initWithObjects:count:]: attempt to insert nil object
  pc [HydrogenVM] CFArrayCreate
  lr  Clash of Clans+1479632 _getIMPublicKeyWithCert + 0x44
  _SecCertificateCreateWithData -> 0   from _getIMPublicKeyWithCert + 0x18
```

`_SecCertificateCreateWithData` was a stub that always returned NULL. InMobi's
tracker (called from `GameMain::init`) then passed that nil to `CFArrayCreate`.
On real iOS `SecCertificateCreateWithData` returns a `SecCertificateRef` for
valid DER; the nil is a HydrogenVM bug, not a game bug. The game next calls
`SecPolicyCreateBasicX509`, `SecTrustCreateWithCertificates`, `SecTrustEvaluate`,
`SecTrustCopyPublicKey`, `SecKeyGetBlockSize` and `SecKeyEncrypt` with
`kSecPaddingNone`.

## Fix

`SecCertificateCreateWithData` parses a DER X.509 certificate and returns a
real CF object (host Security.framework on Apple, OpenSSL on Linux). The rest of
that path is implemented the same way. Trust evaluation does not consult a
system root store: a parsed leaf yields a public key and
`kSecTrustResultRecoverableTrustFailure`. Playback of mixed audio and the
keychain API are unchanged.

Fail-first test in `tests/runner/TestRunner.cpp` (vectors in
`tests/runner/SecCertTestData.hpp`): garbage and empty buffers stay NULL; a
baked 2048-bit self-signed cert (`CN=HydrogenVM Test`) is accepted;
`SecCertificateCopyData` and the subject summary match; the InMobi
CFArray-and-trust sequence produces a 256-byte key; `SecKeyEncrypt` with
`kSecPaddingNone` matches a ciphertext produced by OpenSSL `pkeyutl` (not taken
from the emulator); a too-short raw-RSA buffer returns `errSecParam` (−50). The
test failed on unfixed main (`cert=0`, policy stub `−4`) and passes after the
fix.

## After the fix (60 s, fresh home, no `--internet`)

The game no longer crashes. It presents the title art (barbarian, archer,
Clash shield) and a "Connection error / Unable to connect with the server /
Try again" alert. The log shows "Connecting to the production server..." then
"Connection failed! Error - The Internet connection appears to be offline."

- `crashed`: false, `stillRunning`: true, graceful "The app saved and closed"
- 3482 frames presented, average 16.95 ms, p99 24.90 ms, wall 60.14 s
- Screen 320×480 @2x, landscape (screenshot 640×960, rotate 90° clockwise to view)
- Mixer: `audibleFrames=161792` (~3.7 s), `effectVoices=2`, `musicVoices=0`
  (the Supercell jingle and UI; village music is not reached)
- One StoreKit miss (`Billing::arePaymentsAvailable`): in-app purchases
  disabled, not a crash
- Unimplemented imports still unused: `backtrace_symbols_fd`, `getnameinfo`

The only button on the alert is "Try again", which retries the server. There
is no offline village or tutorial. The client is hard-wired to
`game.clashofclans.com:9339` (2012 Titan protocol). This bench's resolver
sinks outbound names to 198.18.0.1, so a live login was not attempted here;
a 2012 client is not expected to complete login on today's protocol either.
`--internet` is off by default and was not left on.


## Recheck: existing Clash sandbox on main (#414, #391)

Before leaving the blocked verdict, main was searched for a local server the
1.70 client could be pointed at (`clash`, `9339`, `Titan`, `forge`, `sandbox`,
`magic`, `clashofclans`).

- Merged #414 (`codex/clash-forge-runtime`) and closed #391 (Card Forge, Spell
  Lab) are **Clash Royale** guest packages (`com.supercell.scroll` 2.1306.1):
  sandbox editors, custom spells and menu music. They do not implement a Clash
  of Clans village or the 2012 Titan protocol.
- `ClashOffline` (`src/runtime/offline/ClashOffline.cpp`) is a session-owned
  localhost service for that pinned Royale executable only (SHA-256
  `3519d7954cb3dc16b9d7746cc5dbd2521d0209eea2bbc163f6ab7cdfa5e52397`).
  `Process::load` throws `Offline Clash requires the unchanged pinned 2.1306.1
  executable` for any other binary. Confirmed against this 1.70 IPA with
  `--offline-clash`. The host app only auto-enables the backend for
  `com.supercell.scroll` 2.1306.1. The wire path requires login version
  `2.0.1306` and speaks Royale messages (`10101` / `20104` / `24101`).
  `--loopback-alias game.clashofclans.com` cannot attach this service to 1.70:
  the pin refuses to start it.
- The per-game `--internet` switch (#424) does not create an offline village.
  This bench's resolver sinks outbound names to 198.18.0.1, so a live
  `game.clashofclans.com` login was not attempted; a 2012 client is not
  expected to complete login on today's protocol either.
- The TLS bridge (#429) applies only to guest TCP port 443. Clash of Clans
  1.70 talks Titan TCP to port 9339.

The blocked verdict stands.

## Gates (Linux bench, this branch)

| Gate | Result |
|---|---|
| `hvm_tests fulltest` | **814 passed, 0 failed** (one new SecCertificate check; failed first on unfixed main) |
| `hvm_support_tests` | **47 passed, 0 failed** |
| stub audit | **0 new/changed** (673 unlabelled; eight Security stubs became real implementations) |
| `python3 -m pytest -q tools/tests` | **399 passed, 5 skipped**; one pre-existing failure on main after #445 (`test_t4_support.py::TestCensus` looks under `$HVM_FIXTURE_ROOT/ipas/` while this bench's store is `$HVM_FIXTURE_ROOT/` itself). Not caused by this change. |
| Test Lab | **35 passed, 0 failed** |
| `must_launch.py --apps ~/bench/apps` | **1 passed** (Island), **0 failed, 34 skipped**. Clash of Clans is not recorded: it is not fully playable. |

No baseline was weakened. Required CI on this commit has not been reported green.

## Phone-only / not checked

- No iPhone run. The Security path uses Security.framework on Apple (already
  linked) and should behave the same, but that is unconfirmed on device.
- Mesa vs device GPU, real-time speed, 16 KB pages, headphone/microphone
  routing, and a real `game.clashofclans.com:9339` login were not exercised.
- This game does not use the microphone.
