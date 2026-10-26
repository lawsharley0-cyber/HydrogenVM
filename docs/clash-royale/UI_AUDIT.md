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
| BOOT-01 | Fresh tutorial | v27 fresh import starts original tutorial, 20 s, exit 0/no crash; original assets retained |
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
| H-04 | Training progress / Battle | v26 completes seventh earned trainer; v27 native refresh graduates the same save to Arena 1 |
| H-05 | Four empty chest slots | v23 tapped all four: native empty state, no crash |
| H-06 | Earned chest info/unlock/paid/open/reveal | v23 fourth earned chest unlocks after native timer, opens/reveals Gold +16, Mini P.E.K.K.A. and Prince; paid path pending |
| T-01 | Shop tab / available offers / dismiss | v23 original Arena-1 lock renders; offers inaccessible and unqualified |
| T-02 | Cards tab / all eight card tiles | v23 19-state plan captures native info and closes for all eight |
| T-03 | Each card's info / upgrade / use / swap / close | Four real upgrades pass (Archers/Bomber/Arrows/Prince); Knight insufficient cards preserves wallet; native Use swaps Mini P.E.K.K.A. for Arrows and retains across two launches |
| T-04 | Battle tab / arrows | v23 Battle tab returns safely; arena preview covered; progression arrows pending |
| T-05 | Social tab / locked-training notice | v23 original Arena-1 lock renders; unlocked features unqualified |
| T-06 | TV tab / replay / locked-training notice | v23 original Arena-1 lock renders; replay recording unimplemented |
| B-01 | Eight card deployment slots / targeting / invalid placement | Klaus defeated using seven distinct cards from rotating eight-card deck; Prince attempt after victory is not deployment proof; placement failures still pending |
| B-02 | Battle result OK / draw / loss / return home | Seven earned wins; Boris defeat persists twice without a grant; final Silver label matches chest ID 7; real draw previously grants nothing |
| LATE-01 | Initial name / training 4–7 / Arena 1 | Native name/seven trainers pass; v29/v31 actual Arena-1 wins, 65 trophies and Silver rewards; three crowns transfer once; unlocked inner controls still pending |

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

## Subsequent earned training and initial name

The same QA player wins Teemu (fifth) and Tim (sixth). Chest 5 adds 18 Gold
(92→110), one Archers and one Musketeer; chest 6 later adds 16 Gold (110→126),
one Knight and one Archers. Wins/cycle 6 persist, nine cards retained. No save
reset or imported wins. `/var/tmp/hvm-clash-npc5-v23` contains both battles and
intentionally records a failure on returning from Tim: ordinary main 6884670
lacks CTLineGetOffsetForStringIndex when the native initial-name field renders.
That API already exists on this branch. Existing TL-N independently uses the
authored CC0 font's design-unit caret offsets; it fails on the ordinary host
and passes in the branch's 17-case suite. This is a generic runtime dependency,
not a requirement for a Clash-specific backend. The branch also includes the
MS ADPCM decoder absent from 6884670; do not label these two hosts equivalent.

On the updated runtime, v23 then reproduces a separate guest abort on request
10212. v25 implements the native initial-name command 201 via 24111, including
the original fixed BE change-state integer, user flag and server/base metadata.
The original executor applies it; the echo is accepted only after matching the
actual persisted native name/flag. Paid renames remain explicitly unsupported.
`name_ui_plan.json` captures the original dialog, submit and stable home;
`/var/tmp/hvm-clash-name-after-v25` exits 0, no crash/load error, and logs actual
native completion/checkpointing. Relaunch retains the chosen default fixture
name without repeating the dialog. Editing/Unicode keyboard entry still needs
device testing; CLI currently injects touch, not text.

The player upgrades Musketeer natively (50 Gold, 6 XP), giving Gold 76, XP 8,
King level 2. Boris's battle in `/var/tmp/hvm-clash-npc7-v25` is an actual
overtime defeat: original win count stays 6, loss count becomes 1, no chest or
reward appears, and OK returns to home. Exit 0, no crash/load error after
540.208 seconds. v26 adds reset of pending initial-name transport bookkeeping
on reconnect; the saved native named flag remains authoritative. Its relaunch
retains the loss, Gold 76 and upgraded deck. No fabricated loss or win is used.

v26 private IPA SHA256:
`1297cdb2bd505c6973fd68cc7180e6a26aed7a22066407b91aa5c7e0b3a01080`;
executable SHA256:
`4355d2eceef59644710e90d5b5dff9300b26efe90c2cbce9590ddfd0fd080d91`.
Same bundle ID/save format and unchanged original assets. Seven authored guest
tests pass again, including normal/profiling ARMv7 payload compilation.

## Apple validation correction (details)

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

## Integrated build and final training audit

Main 5968d88 is integrated at 5fbaa8d (only the Net.cpp conflict required manual
resolution: preserve main's UDP destination policy and the continuation's TCP
loopback override). The clean WSL source checkout is
`/var/tmp/hvm-main-integrated/source`, build sibling `build`; its Git commit is
accurate, unlike the older manually synced mirror. Fulltest 256/256, TestLab
17/17, native state/service/observer/heap checks, full compatibility and previous
report comparison pass. Indexed gate:
`docs/dev-system/reports/linux-x86_64/20261026T040954Z-5fbaa8d57a17.json`.
Initial fresh-build gate failures were missing standalone ARM test binaries;
after building them, the complete gate has zero regressions. No expectations
or baseline were weakened. Full report: build 1/0, CPU 15/0, iOS 6/0, bench 5/0,
no skips; CoreMark 2365.9 is still below the iPhone-5 goal (a performance limit,
not a crash). The standalone PCM workflow exposed an omitted MsAdpcm.cpp link
input; adding it restores all 42 checks in normal and ASan/UBSan builds.

v26's real sustained Boris battle on this runtime captures 31 states and a
native victory/Silver Chest. Wins 7, losses 2, Gold 76, gems 100, XP 8/King 2,
nine cards and chest ID 7 persist. The 241.187-second run has no crash/load
error, 10,627,072 captured stereo frames, 10,227,828 non-silent, zero transport
errors/dropped blocks. Mixed peak reaches 1.0; signal continuity is not a
perceptual fidelity or clipping-quality pass. Music/SFX mute/restore also
repasses on the integrated host. Desktop percentiles remain slower than the
owner's phone and are not a demonstrated performance improvement.

v26 saves 7/7 but remains at Training Camp. Its next Battle requests mission 7
(an eighth trainer) and correctly trips the unported mission guard. Original
battleEnded calls refreshArena at 0x14c7e immediately after the intercepted win
setter; our early copied snapshot omitted that continuation. v27 invokes the
original refreshArena (0x9b708) on the saved copy after banking the chest. The
same native function also repairs already-earned saved state at login, using
actual CSV NPC thresholds/trophy/demotion rules. No forced arena, score or win.
The existing earned save now persists arena 54000001, shows Goblin Stadium's
new-arena preview and original Arena-1 home, with the same Gold/cards/Silver.
The 180-second v27 run exits 0 without a crash. Fresh v27 startup also passes.
The new-arena legal alert's Linux fallback text has visible glyph/layout issues;
its Okay control and preview Close return safely. Keep that visual bug open.

The UI probe now waits for the first frame before its settling delay. A cold
IPA import previously exceeded the fixed delay, causing the probe to inject
stop before app startup and produce a false no-frame result. It now has a
bounded 90-second first-frame timeout, detects early exits, and never retries
or resets a save. A warm audio recheck and the sustained battle both render.

[Apple CI 40264602540](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40264602540)
confirms the native ISO-language truth table, reports az-Arab=1 diagnostically,
and passes Apple engine/state/TL-O/P/Q checks plus device/simulator compilation.
Its green conclusion includes continue-on-error GPU presentation and batch
console probes that report failures; it is not an all-simulator or device pass.
The integrated 5fbaa8d Apple build also completes (40267946912): native checks,
device/simulator compilation and unsigned IPA packaging pass. Its simulator
console probes still report four continue-on-error failures: frame 60/600,
batch exit marker and landscape frame 60. Green CI is not all-simulator or
phone qualification. Phone retest, unlocked controls, timers and later arenas
remain.

## Arena-1 paid matches and durable crowns

v29 native command 537 starts an original Arena-1 bot match. First abandoned
entry changes Gold 76→75, no result grant. Reopening preserves it. The next
entry changes 75→74; actual Jonas victory gives ELO 34, total wins 8/losses 2,
Silver Chest ID 8/data 19000009 and the matching native Silver reward label.
Private audits: `/var/tmp/hvm-clash-arena1-v29`, `arena1-audit-v29`,
`arena1-return-v29` (all prefixes `/var/tmp/hvm-clash-`). The first win's crown
accounting was missing; it is not silently inferred/regranted.

v30's new crown wrapper incorrectly dereferenced an optional null argument.
Native disassembly at 0xd756c proves the argument is null by design. Its result
guard aborts before a win is committed, leaving Gold 73/pre-result state safe.
v31 qualifies battle.account instead; the next actual match pays Gold 73→72,
wins with three crowns, raises native trophies 34→65, banks Silver ID 9 and
preserves two losses. Original crown getter reports 3/pending 3. On reopening,
the original home transfers pending 3→0 and renders 3/10. Second launch keeps
3/10, 65 trophies, Gold 72, same cards and three unstarted Silver chests, without
duplicate credit/reward. Checksums valid, no journal, no restore/reset.

Evidence: `/var/tmp/hvm-clash-arena1-crowns-v31` (32 states, clean exit,
10,725,376 stereo frames, 10,395,905 non-silent, zero drops/transport errors),
`/var/tmp/hvm-clash-arena1-crowns-return-v31` and `return2-v31` (same prefix).
Screenshots were reviewed: original victory/Silver label, Gold/trophies and
native crown 3/10 home/tooltip. PCM peak 1.0 is not a perceptual-quality pass.
Fresh v31 also starts the original tutorial for 25 seconds and exits cleanly
(`/var/tmp/hvm-clash-fresh-v31`); it does not reuse the earned player's home.
Eight guest tests and 720 private sector comparisons pass. Later bot scenes
are component coverage only, not earned progression; draw/partial-loss crown
qualification, long timers and all unlocked buttons remain open.

## Stutter investigation (measurements)

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
