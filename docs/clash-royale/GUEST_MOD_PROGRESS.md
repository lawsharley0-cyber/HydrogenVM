# Guest-local offline mod — October 25, 2026

## Goal and distinction

The owner explicitly requests moving the offline functionality into a separate
Clash IPA, imported into ordinary HydrogenVM, with its own persistent instance
across emulator updates. This authorizes modifying a **copy** of the pinned
game executable. The original IPA/executable and all assets remain unchanged.
No proprietary executable, generated fixture header, modified IPA, credential
or player save belongs in Git.

This supersedes the metadata-only packaging experiment as the intended final
architecture. `package_clash_offline.py` still requires the embedded host
service. `build_clash_guest.py` builds actual ARMv7 guest code into the client;
it does not set `HydrogenVMOfflineBackend`, start a TCP server, use Frida/JIT,
or call HydrogenVM's Clash service. Ordinary emulation/framework/file APIs are
still required, as for every guest app.

**Prototype, not a complete replacement yet.** Training startup, battle input,
three earned training victories, chest banking, home persistence, native
reward recovery and a paid card upgrade have been reproduced. Selected deck and
tower level adaptation are implemented. Full training/arenas,
elapsed closed time, paid/free/crown qualification, name flow,
intentional interruption tests and device validation remain open.

## Reproduction and packaging

```sh
python3 tools/build_clash_guest.py /path/to/pinned.ipa /private/out/prototype.ipa \
  --build-dir /private/new-build-directory
hvm run /private/out/prototype.ipa --home /private/player --run-for 45 \
  --graceful --gpu --report /private/startup.json
```

The build uses clang++, ARMv7 clang and cctools ld64. It generates initial data
from the owner's pinned assets using the existing `ClashFixtures` serializer,
then compiles a freestanding guest payload. Only the output executable and
Info.plist change; every other archive member is compared byte-for-byte.
New build/output paths are required; existing data is not overwritten.

The development bundle ID is `com.hydrogenvm.clashroyale.guestdev`, display name
`Clash Offline Guest DEV`. It intentionally differs from the original client
and the earlier host-dependent package. Keep the bundle ID stable while
testing updates. Documents are preserved on re-import by the normal importer;
updating the emulator preserves the instance only when its own app identity
and data container are retained. This is not a directly installable modern-iOS
ARM64 app or a signed package.

## Implementation

- `tools/clash_guest/transport.c`: build-time replacements of Messaging connect,
  send and disconnect. Native connected state now represents a local message
  queue. Original factory/decoders create response objects, and original queue
  enqueue delivers them. No blanket network-success stubs or checksum acks.
- `fixture_export.cpp`: private initial home/login and seven training-sector
  templates. `sector.inc` inserts the saved selected deck and avatar, rebuilds
  native hand/queue indices, and applies original CSV tower level/HP values.
  The pinned client has twelve experience levels. Unique field matches and
  fixed VInt widths are checked at build time; all writes are bounded.
- `save.inc`: CRG1 native-home checkpoints in
  `Documents/ClashGuestOffline-v1.bin`. The header is four little-endian words:
  magic `0x31475243`, body size, avatar offset, FNV-1a body checksum. Body is the
  original serialized home/avatar plus terminal boolean. Same-directory temp,
  checked writes/flush/sync/close, then rename; invalid data fails without reset.
- Optional journal: five little-endian words after the body: `0x314a5247`,
  command size, source kind, chest ID, FNV-1a command checksum; immutable native
  command bytes follow. Old files without the extension remain readable.
- Original GameMode update prologue is relocated into an argument-preserving
  thunk; native home snapshots run before the unchanged original function.
  No host Engine observers are used.
- Native NPC win setter retains its store and observes only the original
  `LogicBattle::battleEnded` return address (`0x14c7c`, Thumb bit ignored).
  Serialized copies are decoded with OwnHomeDataMessage, and original
  getNextWinnerChest/LogicChest/addChest APIs bank the chest. Winner index and
  chest count advance on the **copy**, so victory UI still shows this reward.
  Home plus updated avatar commit in one file. Full slots do not advance cycle.
- `home_turn.inc`: validates local home echoes. StartExploring **502** starts
  the timer; StartRewardClaim **503** requests collection. ClearChestSource 522
  clears the native new badge. Server command 213 echoes use server executor
  identity, not client identity; decode them with the original class and match
  the qualified reward source, without executing them again.
- `reward.inc`: original randomizer/command 213 encoding, journal-before-queue,
  and original execute trampoline. The cloned, validated current claim alone
  is unclaimed in its checkpoint; native gem charges/unlock state survive.
  The native execute result and source qualify clearing the journal. Pending
  journals block snapshots until success, replay after home on login, and
  clear atomically with the native granted state.
- The ARM import stub for `__sincosf_stret` targets a guest C struct-return shim
  using scalar sinf/cosf. This removes dependence on the old host pair-return
  ABI bug for cannon recoil. No cannon positions/angles are frozen. Full visual
  motion and phone correctness still need comparison.

Unsupported gameplay requests fail explicitly. Native assertions, bounds
checks, game assets, original combat and NPC AI remain intact. Empty offline
friend/mail/TV/replay lists use their actual pinned native message schemas;
they do not stand in for implemented online social/replay features.

## Evidence and limits

The ordinary test host is an independently built clean archive of main
`6884670` at `/var/tmp/hvm-standalone-main/{source,build}`. That revision has no
ClashOffline source/service. All runs below omit offline and loopback flags.
Its source archive and all private evidence remain outside Git.

- v2 deliberately stops on a pre-login keepalive; native MessageManager can
  send it while CSV loading delays login. v3 handles it and loads own home/NPC1,
  then exposes the missing friend-list request.
- v4 runs 45 seconds to the original Trainer Earl welcome and exits cleanly:
  `/var/tmp/hvm-guest-v4-evidence`. A drag exposes unsupported UI event 12951;
  bounded handling consumes opponent-facing visuals, not authoritative commands.
- v5 renders real deployed troops and destroys a Princess/King tower with native
  combat on main. This earlier version has no guest persistence qualification.
- v7 wins NPC1, records native count 1 in its own file and survives process
  restart. The original client accepts the saved home and enters HomeScreen;
  unsupported home requests then stop it. No backup is restored. Private
  evidence: `/var/tmp/hvm-guest-v7-{battle,evidence}`.
- v9 fresh player wins NPC1. Original chest selection banks the Wooden Chest
  shown on victory. Saved home has count 1, unique chest ID 1 and native cycle 1.
  `/var/tmp/hvm-guest-v9-battle`; player `/var/tmp/hvm-guest-v9-player`.
- Later mod imports use that same player, retaining its Documents. v10 exposes
  ClearChestSource 522. v11 exposes StartExploring 502. Both are now decoded.
- v12 naturally unlocks the 15-second Wooden Chest and journals a real 35-byte
  grant, then aborts after native success because its 213 echo incorrectly
  passed through client-identity validation. v13 recognizes the echo but uses
  full stream length instead of read offset and fails its boundary check.
  v14 uses native getOffset `0x192b44`; it recovers the **same journal** across
  these aborted processes without restoration: Gold 100→116, gems 100, Knight/
  Arrows reveal, chest gone, journal clear and NPC win 1. It does not grant Gold
  132/148. This is actual aborted-process recovery; a deliberate SIGKILL run
  still needs qualification. Evidence `/var/tmp/hvm-guest-v14-evidence`.
- Opening Cards in v14 exposes command **539**, LogicPageOpenedCommand.
  Native decode `0xd8af8` reads one VInt; execute `0xd8b10` validates pages 1..4.
  The guest now qualifies this echo and snapshots original native execution.
- v17 retains the same earned player: first reward Knight/Arrows counts are
  visibly 2/2. Native Knight upgrade spends 5 Gold (116→111), gives 4 XP and
  raises the Knight to level 2. Trainer Fikova starts with the saved upgraded
  six-card deck, native combat destroys the King, and the second earned win
  banks original chest data 19000001, ID 2, cycle 2. No save restoration or
  invented battle outcome is used. `/var/tmp/hvm-guest-v17-evidence`.
- Private sector comparison: **252 byte-exact component checks** against the
  existing C++ serializer (7 trainers × 12 tower levels × 1/6/8 card decks).
  Synthetic derivatives stay under `/var/tmp/hvm-guest-v17-sector-oracles`,
  never replace the earned save, and are not earned progression. These vectors
  neutralize the native show-new-count and card UI flags omitted by the C++
  Card projection. Actual guest sectors retain those fields and mask out only
  subsequent home-deck presence bits. `sector_verify.c` and the exporter's
  optional checkpoint oracle mode make comparison reproducible privately.
- Two subsequent clean 35-second relaunches retain Gold 111, gems 100, XP 4,
  NPC wins 2, cycle 2 and the same unopened chest ID 2, with no pending journal.
  Both terminate through the original save/close path without an abort:
  `/var/tmp/hvm-guest-v17-relaunch{1,2}`. This does not yet qualify killing a
  process during journal commit or a real phone update/re-import.
- v18, built from `ee23825`, normally unlocks the second earned 15-second chest,
  commits a 35-byte journal, executes original command 213, and clears the
  journal with the granted home. Gold 111→127, gems 100, XP 4, NPC wins 2;
  the native deck now contains seven cards, including Musketeer (26000014).
  A 110-second mixer capture records 2,371 blocks, 2,276 non-silent, peak 31,974,
  no capture errors; run exit 0, no crash/load error. This confirms mixed output
  during interaction, not isolated music correctness or phone dropout resolution.
  `/var/tmp/hvm-guest-v18-audio/{report.json,audio.json,mixed.wav,run.log}`.
- v19 adds the original loss setter store (+0xa0), qualified only at the native
  battleEnded return address 0x14ce6 and local avatar ID. It commits the original
  avatar without awarding a chest. An idle Trainer Mike battle naturally reaches
  the original DRAW screen; it awards no win/chest and is not a defeat test.
  A real loss invoking the new setter remains unqualified. The next native
  Trainer Mike attempt uses the seven-card saved deck.
- That second Trainer Mike attempt earns a real three-crown win: NPC wins 3,
  native cycle 3, banked chest data 19000003 with ID 3. The same player is
  retained throughout. The original 600-second probe stops gracefully during
  unlock; its native timer remains on disk (closed-time advancement is still
  unported). `/var/tmp/hvm-guest-v19-loss` records both draw and earned win.
- A separate owned v19 run finishes that unlock, then a private pidfd watcher
  sends **SIGKILL** only after verifying the atomically renamed checkpoint and
  pending command checksums. The retained 35-byte journal is source 1 / chest
  ID 3, Gold 127, wins 3, claimed=false. No backup is restored. The next launch
  loads it, executes original command 213 once, and commits Gold **141**, gems
  100, XP 4, wins/cycle 3, no chest or journal. The native deck now has eight
  earned cards, including Prince (26000018); Bomber count is 2. The earlier
  Mini P.E.K.K.A label was a documentation error, corrected by inspecting the
  original pinned card-info screen during the UI audit; no card data changed.
  Evidence `/var/tmp/hvm-guest-v19-journal/interruption.json` and
  `/var/tmp/hvm-guest-v19-recovery1`.
- A second clean 35-second launch retains Gold 141, gems 100, XP 4, wins/cycle
  3, the same eight cards and no chest/journal. No reward executes again. Both
  recovery reports have exitStatus 0, crashed=false and empty loadError.
  `/var/tmp/hvm-guest-v19-recovery2`. The private v19 IPA SHA256 is
  `937e8e5d0c03c176776eac9537b06d6cc73491dca2d33fcb4df599d6d7bd21c3`.

The local mod builder's seven tests use authored Mach-O vectors, independently
specified signed VInt boundary vectors and a synthetic fixture header,
including a real freestanding ARMv7 compile. They require no
proprietary IPA. CI runs them. They do not prove a playable game by themselves.

The separate host-backend checkpoint `662da07` passed Linux and Apple CI in
[run 40167767892](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40167767892).
Its downloaded unsigned ARM64 device IPA was locally reverified (SHA256
`f9d106516d815d68aba2ced0a92e8bb1ed1b2d151e01e615b9096bb96e099a47`, minimum
iOS 26.0). It remains untested on the owner's phone; neither that build nor the
guest prototype is proof the reported device 50% failure is resolved.

## October 25: phone abort diagnosis and interactive audit

The owner's phone v19 report exactly identifies missing native home command
516 after a king-level-up sound. The new guest accepts its base-only wire
schema and retains original execution to update last-shown level. A synthetic
level-2 QA copy reproduces v19's abort and v20 reaches home for 35 seconds.
The phone's failing save is not reset; matching device re-import is pending.

v23 also adds local offline help/account/backup/legal notices in the original
InfoPopup and serves the local profile through native 14113/24113 messages.
It runs on ordinary, unmodified main 6884670 without a host Clash service.
Its private IPA SHA256 is
`4264518926857d2e9aa1b891072936506c18cbc87e56cb761b90c5319f813a8b`;
executable SHA256 is
`5a9aeb1fd4a7378a667c41648d4f1bb4327d525cf842e58a2c071072c3bb297a`.
Bundle identity and CRG1/GRJ1 persistence stay stable; original assets are
byte-verified unchanged. It remains an ARMv7 emulator guest, not a native app
for modern iOS. Do not commit or distribute proprietary fixtures in Git.

An isolated copy of the three-win earned player now passes all eight card-info
screens and closes, a blocked Knight upgrade without spending, and three real
Archers/Bomber/Arrows upgrades. Gold 141→126, XP 4→16, all three reach level 2.
Profile stats/deck, scrolling, arena preview and resource tooltips render.
Shop/Social/TV are still natively Arena-1 locked; their inner controls have not
been tested. Audio PCM confirms independent music/SFX mute and restore with
zero dropped mixer blocks in that check, not full battle perceptual quality.
The complete scenario ledger, known gaps, commands and performance measurements
are in [UI_AUDIT.md](UI_AUDIT.md); machine captures are not automatic visual passes.

The support SDK independently exposed missing shared NSPredicate MATCHES and
NSLocale character direction. Linux now uses ICU; Apple uses native Foundation
and CoreFoundation. Minimal TL-P/Q cases fail before and pass after. Final
Linux fulltest: 253/253, TestLab: 17/17. Complete compatibility baseline diff:
zero regressions; three legacy apps start without crashing, original optional
Clash fixture skipped in that gate (separately exercised above). An additional
two-launch smoke of each legacy app exits cleanly. Its apps-only report cannot
be compared to a whole-suite baseline: that preliminary diff flags omitted
unit suites, not a measured runtime regression. Apple compile/tests remain CI
gates. The new guest facade also avoids the SDK's unimplemented Auto Layout;
this change does not claim a general Auto Layout implementation.

Synchronous checkpoint I/O is being measured, not disabled. An optional
`--profile-saves` build logs checkpoint I/O duration without weakening fsync,
journal or rename behavior. Local software-rendering/tracing timings cannot
establish the owner's phone hitch cause. Newest fetched main is 5968d88 and
includes a separate GPU/AUGraph fix f78a63a; these current runs use the explicitly
identified 6884670 ordinary host, not that newer integration head.

Later in the same audit, the private earned QA player defeats trainer Klaus
(4/7), banks and unlocks chest ID 4, collects 16 Gold plus Prince/Mini P.E.K.K.A.,
and swaps Mini P.E.K.K.A. for Arrows using native Use. Two launches retain the
nine-card collection, upgraded cards, resources and swap with no duplicate
reward. A further native Prince upgrade costs 50 Gold and grants 6 XP, naturally
reaching King level 2 (Gold 92, XP 2). Relaunch shows the original King-level-up
screen; Close returns to home and profile safely. This is actual earned level-up
evidence in addition to the synthetic fixture, still not phone qualification.

Apple CI 40263193112 passes the engine build/fulltest, state checks and TL-O/P,
then fails TL-Q's initial nonportable script-tag assertion. The follow-up uses
the documented ISO language-code contract (English/Arabic/Hebrew/Persian/Urdu),
removes Linux's likely-script override, and adds an independent native Apple
truth-table executable. Corrected Linux fulltest 253/253, TestLab 17/17 and the
complete zero-regression baseline diff pass. Next Apple CI must still complete.
See UI_AUDIT for the primary-source contract rationale; expectations are not
derived from emulator output.

## Next work

1. Preserve the deliberate SIGKILL recovery evidence and earned eight-card
   player. Do not reset or restore older checkpoints during the next tests.
2. Qualify an actual eight-card battle, deck swaps and level-up tower stats;
   synthetic byte comparisons do not substitute for those scenarios.
3. Qualify NPC4 onward, name flow and all seven earned training wins. Qualify a
   real loss; port match costs, actual crowns, arena transition and native ELO.
4. Port arena/bot selection and all six locations, real elapsed timers and the
   remaining native economy/journal paths. Build-time data can reuse the pinned
   CSV research, but runtime logic must stay in the guest IPA.
5. Use an ordinary main device build to qualify login/gameplay/audio/visuals.
   Keep the original phone reports open until matching scenarios pass.
