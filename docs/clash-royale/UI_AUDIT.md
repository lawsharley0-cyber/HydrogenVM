# Offline guest UI audit — October 25, 2026

The owner requests every clickable function checked, repeated post-upgrade
launches, and stutter profiling. This is an active coverage ledger, **not a
claim of complete or crash-free functionality**. Locked/unreached controls and
online-only actions must be identified explicitly. Keep the earned player and
the phone's failing instance intact. Use separate QA copies for upgrades and
synthetic component fixtures; never substitute them for earned progression.

## Matching phone evidence

The owner's report confirms v19 executable SHA256
`3d8b2e209470b52e779811d5acdc184bd65303817f134cb2a95d712c213a3bd5`,
host `68846700e5884534b63ba785ded21dd602f79264`, iPhone18,2, iOS 27.2,
16 KB host pages. Home opens, king-level-up sound plays, command 516 reaches
the guest's unsupported-command guard and aborts. This is a guest implementation
gap; the report is not evidence of a corrupt save or a CPU/renderer fault.
No phone container identifier, credentials or raw player data belong in Git.

The 4.479-second startup report has p50 15.47 ms, p95 32.77 ms, p99 58.20 ms,
maximum 160 ms; 65/215 frame hooks exceed 16.7 ms. It mixes startup and home,
so it cannot establish a steady-gameplay stutter cause.

## Reproduced fixes and limitations

- Native command 516 is LogicUpdateLastShownLevelUpCommand. Its encode/decode
  at 0x1109bc/0x1109c0 contain only LogicCommand base metadata. Execute at
  0x1109c4 updates the native home last-shown level. Guest and C++ turn decoders
  now qualify it and preserve original execution/checkpointing. The new literal
  schema test fails before acceptance; identity/truncation/trailer checks remain.
- A separate **synthetic** level-2 home repeats the exact v19 516 abort. v20
  reaches home for 35 seconds on ordinary main 6884670, without modifying or
  resetting the original earned player. Actual phone re-import remains pending.
  `/var/tmp/hvm-clash-levelup-{before,after}`.
- Original Help/Support independently fails on `SELF MATCHES %@`. Shared
  NSPredicate support now uses ICU on Linux and native Foundation on Apple,
  including inline flags/Unicode categories, full-input matching, argument arrays
  and invalid-pattern errors. TL-P fails before and passes after; an initial
  ECMAScript experiment was insufficient for the support SDK's Unicode email
  pattern and was replaced. No validation is bypassed.
- The original support SDK next needs NSLocale character direction and UIKit
  Auto Layout (`UIView addConstraints:`). Locale support uses native
  CoreFoundation on Apple and ICU script/layout data elsewhere; TL-Q checks known
  English/Arabic/Hebrew/Persian/Urdu directions. Full Auto Layout is not
  implemented in this change.
- The **offline edition** supplies authored local help in the game's original
  InfoPopup instead of pretending to connect the archived online support SDK.
  v21 exposed text overflow; v22 shortens the content to fit above OK. v22's
  25-state settings plan exits cleanly with native credits, local help, account,
  device-transfer and legal-page notices. v23 repeats the final settings plan
  after three real upgrades; all 25 states exit cleanly and final help/account/
  backup/legal screenshots fit above the original OK button.
  Generic shared API fixes still benefit other apps.
- v22's profile button exposed missing VisitHomeMessage 14113. Native encode
  at 0x3d78c writes the eight-byte LogicLong. The guest accepts only its own
  player identity, captures current native home/avatar, and supplies original
  VisitedHomeDataMessage 24113 (VInt zero + saved home). v23 renders and scrolls
  the original profile and closes safely; unsupported remote IDs still fail.

## Control inventory

Status is scenario-specific: `pending`, `observed`, `passed`, `blocked`, or
`failed`. An unchanged frame after a legal-page click is not a functional pass.

| ID | Screen / control | Current evidence |
|---|---|---|
| BOOT-01 | Fresh tutorial | Earlier guest startup passed; rerun final package |
| BOOT-02 | Returning earned player | v19 three wins/eight cards retains save; QA clone opens |
| BOOT-03 | Post-king-level-up home | v23 earned King level 2 relaunch shows native level-up screen, closes and opens profile without abort; synthetic fixture also survives two final-package launches |
| BOOT-04 | Re-import/relaunch after upgrades | v23 two clean launches retain four wins, upgraded cards and deck swap; phone still pending |
| S-01 | Settings open / close | v22 passed open/close and final ON/ON/OFF settings |
| S-02 | Music off/on | v23 PCM idle tail: ON peak 6786, OFF 0, restored 10928 |
| S-03 | SFX off/on | v23 music OFF: replay toggle ON peak 10868, muted 0; switches restored |
| S-04 | Replay landscape off/on | v23 native setting toggles safely; actual replay feature unported |
| S-05 | Facebook | v22 explicit offline-account notice opens/closes |
| S-06 | Credits | v22 original scrolling credits renders; tap closes safely |
| S-07 | Link Device / choices | v22 explicit local-backup guide; cloud transfer unavailable |
| S-08 | Change Name | Native level-4 requirement floater appears; unlocked name flow pending |
| S-09 | Help and Support / close | v23 final short local help fits; opens/closes without crash |
| S-10 | Privacy | v22 explicit offline-page notice opens/closes |
| S-11 | Parents Guide | v22 explicit offline-page notice opens/closes |
| S-12 | Terms of Service | v22 explicit offline-page notice opens/closes |
| H-01 | Profile / clan label | v23 original local profile, deck/stats, scroll and close passed; clan functionality locked |
| H-02 | Experience, Gold, Gems | v23 all three native tooltips open/dismiss with correct values |
| H-03 | Arena preview / close / scrolling | v23 native preview opens, scrolls, closes |
| H-04 | Training progress / Battle | v23 actual fourth trainer Klaus defeated with upgraded eight-card deck; native progress 4/7 |
| H-05 | Four empty chest slots | v23 tapped all four: native empty state, no crash |
| H-06 | Earned chest info/unlock/paid/open/reveal | v23 fourth earned chest unlocks after native timer, opens/reveals Gold +16, Mini P.E.K.K.A. and Prince; paid path pending |
| T-01 | Shop tab / available offers / dismiss | v23 original Arena-1 lock renders; offers inaccessible and unqualified |
| T-02 | Cards tab / all eight card tiles | v23 19-state plan captures native info and closes for all eight |
| T-03 | Each card's info / upgrade / use / swap / close | Four real upgrades pass (Archers/Bomber/Arrows/Prince); Knight insufficient cards preserves wallet; native Use swaps Mini P.E.K.K.A. for Arrows and retains across two launches |
| T-04 | Battle tab / arrows | v23 Battle tab returns safely; arena preview covered; progression arrows pending |
| T-05 | Social tab / locked-training notice | v23 original Arena-1 lock renders; unlocked features unqualified |
| T-06 | TV tab / replay / locked-training notice | v23 original Arena-1 lock renders; replay recording unimplemented |
| B-01 | Eight card deployment slots / targeting / invalid placement | Klaus defeated using seven distinct cards from rotating eight-card deck; Prince attempt after victory is not deployment proof; placement failures still pending |
| B-02 | Battle result OK / draw / loss / return home | Native fourth win, Wooden Chest label, OK/return/claim passed; earlier draw gives nothing; real loss unqualified |
| LATE-01 | Name flow / training 4–7 / Arena 1 unlocked UI | Pending progression; do not report these controls passed |

## Test host and private evidence

Ordinary unmodified main host: `/var/tmp/hvm-standalone-main/build/hvm`,
6884670, Linux/WSL 4 KB pages, GPU through desktop EGL. Shared framework changes
use `/var/tmp/hvm-offline-20261023/build` with explicitly synced sources; its Git
metadata is stale, so it must not be labeled a clean current commit. Source and
binary hashes are required in a final machine-readable report.

QA copy `/var/tmp/hvm-clash-ui-qa-player` derives from the three-win local player;
the original `/var/tmp/hvm-guest-v9-player` remains untouched. UI screenshots and
per-run logs live under `/var/tmp/hvm-clash-ui-*` and private `outputs/`.
Before framework changes: fulltest 252/252, Test Lab 15/15. Final regex and
locale changes: fulltest 253/253, Test Lab 17/17; C++ home-state checks pass.
Linux passes do not qualify the newly added native Apple regex adapter; Apple
CI remains required. A passing local run does not resolve device observations.

Private evidence: `/var/tmp/hvm-clash-ui-settings-v22`, `home-v23`, `cards-v23`,
`audio-v23`, `upgrades-v23`. The audio run records 1,695,744 stereo frames,
1,138,072 non-silent, zero dropped blocks/transport errors. This verifies signal
and switch behavior, not uninterrupted perceptual quality during a full battle.
The upgrade plan spends 15 earned Gold (141 to 126), adds 12 XP (4 to 16), and
native Archers/Bomber/Arrows become level 2; the original earned player is intact.

Fourth trainer Klaus: `/var/tmp/hvm-clash-npc4-v23`, 600-second ceiling, stopped
gracefully after victory/chest/reveal/swap. Exit 0, no crash/load error. The
original native randomizer gives 16 Gold (126→142), one Prince and newly owned
Mini P.E.K.K.A. (ID 26000016), total nine cards. Native Use then replaces slot
four's Arrows with Mini P.E.K.K.A.; Arrows level 2 remains in the collection.
`/var/tmp/hvm-clash-ui-relaunch{1,2}-v23` each captures five states and exits
cleanly, retaining resources, four wins, no chest/journal and the swapped deck.
Dragging the full eight-card deck did not reorder it; that exploratory gesture
is not a passed swap test. The verified action is the native Use flow.

The next actual Prince upgrade costs 50 Gold and grants 6 XP in **this client's
unchanged data**, giving Gold 92, XP 2 and King level 2. This is earned progression,
not the synthetic fixture. `/var/tmp/hvm-clash-prince-upgrade-v23` exits cleanly
while the original upgrade animation shows Prince level 2. The same save then
relaunches in `/var/tmp/hvm-clash-earned-level2-v23`, displays the native King-2
upgrade screen (King HP 2592, arena tower HP 1526, both damage 54), closes and
opens the native profile without abort. Further clean launch checks must ensure
the level-up screen is not repeatedly presented after its native acknowledgment.

## Apple validation correction

[CI 40263193112](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40263193112)
passes Linux, Apple compilation/fulltest, offline state checks and TL-O/P, then
fails TL-Q. The initial test incorrectly treats the script tag `az-Arab` as a
portable oracle for an API documented to take an ISO language code. Apple's
[contract](https://developer.apple.com/documentation/corefoundation/cflocalegetlanguagecharacterdirection(_:))
and [published implementation](https://github.com/apple-oss-distributions/CF/blob/main/CFLocale.c)
use ICU character-orientation locale resources, not a likely-script override.
The Linux implementation now follows that mapping consistently. TL-Q instead
checks known English, Arabic, Hebrew, Persian and Urdu ISO codes; the expected
directions derive from their writing systems. A separate native Apple executable
checks the same truth table and prints the tag result diagnostically. This
corrects an unsupported test assumption; it does not copy emulator outputs into
expected values. Corrected Linux fulltest 253/253 and TestLab 17/17 pass; the next
Apple CI must confirm the native comparison and complete Xcode builds. The
corrected full compatibility run also has zero baseline regressions, at
`/var/tmp/hvm-clash-locale-contract-compat`.

Correction: the eighth owned card is **Prince**, not Mini P.E.K.K.A. Native card
ID 26000018 and the rendered title identify it. Earlier notes used the wrong
name; no card data was changed. This client also legitimately shows Musketeer
at five Elixir; modern balance values are not the reference for this build.

## Stutter investigation

The phone report had Zombie Objects enabled. That diagnostic deliberately
retains released objects; measure a normal run separately before attributing
all hitches to the guest. Do not suppress useful crash diagnostics globally.

One traced WSL run on ext4 measured 50 fsync calls in 60 seconds: median 2.230 ms,
p95 5.164 ms, max 7.399 ms. Tracing perturbs timing. The guest currently saves
synchronously about once per native second and on critical transitions. The
optional builder flag `--profile-saves` measures checkpoint file I/O without
changing frequency, fsync, journal or atomic replacement. It is diagnostic,
not a performance fix. Native serialization is outside that timing window.

That local run averaged 36.88 ms/frame with 12.11 ms software composition;
the owner's phone report averages 0.022 ms composition, so this desktop result
does not explain the phone's steady-play hitches. Warm battle measurements,
frame pacing, allocations and main-thread I/O remain candidates. Apple's
[file-access guidance](https://developer.apple.com/documentation/foundation/improving-performance-and-stability-when-accessing-the-file-system)
and [hitch diagnostics](https://developer.apple.com/documentation/xcode/understanding-hitches-in-your-app)
support measuring these costs before changing durability or scheduling.
The host fsync shim currently ignores host errors; disk-fault recovery is not
qualified by these tests and remains separate runtime work.
