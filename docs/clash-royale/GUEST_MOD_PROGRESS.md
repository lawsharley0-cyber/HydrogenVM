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

**Prototype, not a complete replacement yet.** Seven earned training victories,
two actual Arena-1 wins, initial name, paid card upgrades/King level 2, native
deck swaps, chest banking and interrupted native reward recovery are reproduced.
The latest arena victory credits three native crowns; two relaunches retain
3/10 without a duplicate. v33 adds measured closed/paused home time and actual
paid-reward interruption recovery with its atomic clock stamp. The earned Silver
now completes naturally. v42 adds persistent solo-clan core controls; v46 adds
native clan mail, durable Inbox/cooldown and forced-stop recovery. v48 adds
original unfulfilled card requests and their eight-hour cooldown/recovery in labeled
King-3 component fixtures. See LOCAL_CLANS for the exact scope. Later arenas,
draw accounting, remaining economy/control qualification
and device validation remain open. The owner next prioritizes local clans,
post-training randomized opponent names, varied arena-appropriate bot decks and
playstyles, and the original daily shop; see BACKLOG's ordered extension.

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
- `fixture_export.cpp`: private initial home/login, seven training-sector and
  thirteen Arena-1 bot templates. `sector.inc` inserts the saved selected deck and avatar, rebuilds
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
- v33 adds a GRT1 clock extension before the optional journal: four
  little-endian words `0x31545247`, accounted 32-bit Unix seconds, body FNV-1a,
  FNV-1a of the first twelve clock bytes. Body, clock and journal replace in
  one atomic checkpoint. Old unstamped saves start accounting on migration;
  historical time is never inferred from filesystem mtime. Older guest builds
  cannot read this new extension: update forward and retain the instance/save.
- The pinned loadHomeState callback at 0xd6d40 binds clock accounting to the
  actual newly delivered home after native fastForwardTime, before
  loadingFinished. Its ten PC-independent instruction bytes are preserved;
  the following PC-relative ADD runs at its original address. Native ticks
  already represent active time, so only the unaccounted wall-time difference
  goes through original home fastForward (0x125d7c). Pending journals defer that
  step until their native grant completes. Elapsed credit is bounded to thirty
  days and never negative; it does not change battle simulation time.
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

The same player subsequently wins Teemu and Tim (6/7), collects both native
rewards and retains nine cards. Tim's return-home exposes ordinary main 6884670's
missing CoreText caret API at the name field; this branch already implements
it with independently specified TL-N coverage. On the updated runtime the
original name dialog renders, but v23 then aborts on guest request 10212.
v25 supplies original initial-name command 201 and qualifies the actual native
execution/echo/checkpoint. Relaunch retains the name. v26 clears only pending
name-transport bookkeeping on reconnect, leaving native saved state authoritative.
Paid rename and actual keyboard editing remain unqualified. No host Clash
backend/marker/flags are used; generic framework compatibility is still required.

An earned Musketeer upgrade costs 50 Gold and grants 6 XP (Gold 76, XP 8, King 2).
A real overtime defeat against Boris persists losses 1, wins 6, no chest/reward,
and returns home safely. That 540-second run exits 0 with no crash/load error;
v26 relaunch retains the result. Original earned source save remains at three
wins; this extended progression is its private QA copy. See UI_AUDIT for hashes,
source-dependent runtime limitations and exact private evidence paths.

The integrated host 5fbaa8d now includes main 5968d88 and passes 256 runtime
checks, 17 TestLab cases, the complete zero-regression compatibility gate and
the full build/CPU/iOS/benchmark report without skips. The source checkout's
commit is exact; see the indexed JSON linked from UI_AUDIT. Standalone PCM's
new MsAdpcm.cpp link input restores 42 normal/sanitized checks.

A second real Boris loss persists without a grant. The next v26 battle wins
normally, reaching seven earned victories and Silver Chest ID 7. A 241-second
31-state audit captures 10,627,072 stereo frames (zero dropped blocks/transport
errors), then exits cleanly. Save remains Gold 76, gems 100, XP 8/King 2, nine
cards, two losses. Mixed peak 1.0 is not a clipping/perceptual-quality pass.

The first seventh-win snapshot misses original battleEnded's refreshArena call
at 0x14c7e, leaving 7/7 at Training Camp; another Battle requests unsupported
mission 7. v27 invokes the same original refreshArena on the decoded saved
copy and at login for already-earned saves. The actual seven-win save now
persists arena 54000001 and renders native Goblin Stadium/Arena 1 home, without
changing Gold/cards/Silver/wins. Fresh v27 tutorial and a 180-second graduated
relaunch both exit cleanly. The native new-arena legal alert has Linux text
glyph/layout issues still open. Matchmaking 537 and unlocked controls remain
the next guest implementation/audit stage.

v27 private IPA SHA256:
`45f55e8b2d18776ecb094a9cdba21d3b97777b5af90aa8e93956979b0b099471`;
executable SHA256:
`c8388799e17c8bbe8ba2b88d2379d840aa172ab08cb3d1e4b0f42bb170755821`.
The bundle/save identities and original assets remain unchanged. No phone test
or complete-game claim follows from these local results.

## Arena-1 guest battles and crown transfer — October 26

Native command 537 (decode 0xbbf4c) carries base metadata, a selector VInt and
packed boolean. The actual Battle control sends 0/false. The guest validates
the complete turn and debug trailer before starting a match; unknown variants
still fail. Thirteen original non-training NPC templates use the pinned Arena-1
sector/AI data, selected by nearest original NPC trophy count. The original
commodity helper charges the arena's native Gold cost and FreeGold clamp on a
decoded checkpoint copy. Only a fully assembled/compressed sector permits the
atomic debit; aborted/abandoned matches retain their paid entry, without wins
or rewards. No host backend is involved.

v29 wins against original Trainer Jonas, raises native ELO 0→34 and banks
Silver Chest ID 8, whose result label matches. A separate abandoned attempt
costs one Gold without a grant; reopening charges nothing. The next real v31
win raises ELO 34→65, wins 8→9 (seven training plus two arena), losses remain
2 and Silver Chest ID 9 is banked. Gold is 72, gems 100, XP 8/King 2, nine cards;
IDs 7/8/9 remain unstarted, three-hour Silver chests. No save was reset/restored.

The guest's new battleEnded trampoline preserves the original prologue and
argument ABI. It supplies the actual battle context to the result checkpoint:
original LogicSummoner::getStars (0x10f3fc), pending-star getter/setter
(0x9b080/0x9b0a8), then original home transfer. v30 incorrectly inspected the
optional avatar argument; native LogicGameMode::battleEnded explicitly passes
null (0xd756c). Its guard stopped the result before persistence, retaining
only the already-paid entry. v31 qualifies battle.account and forwards the
nullable argument unchanged. No assertion was hidden. The actual v31 victory
reports three crowns; home changes pending 3→0 and displays 3/10. Two clean
relaunches retain the same Gold/trophies/wins/chests/crown count, no journal.
The earlier v29 crowns are not inferred or retroactively granted. Loss crown
copying is implemented but an actual arena defeat with crowns is unqualified;
draw crown accounting still needs its own original-result path.

Eight authored guest tests pass, including real ARMv7 normal/profiling payload
compilation and a C++ fixture-exporter build without proprietary data. A private
component audit compares 720 sectors byte-for-byte to the independent C++
serializer: 20 bots × 12 tower levels × 1/6/8 cards. These comparisons do not
claim all bots/levels were played. The actual v31 battle captures 32 states,
10,725,376 stereo frames, 10,395,905 non-silent frames, no dropped blocks or
transport errors, and exits cleanly. Peak 1.0 and Linux frame timings do not
resolve the owner's phone audio/stutter reports.

Private v31 IPA SHA256:
`dbd502c6789f433727d47aee9c17e8e258ef758ddc728bf2e63ecb152ae58ced`;
executable SHA256:
`e8c5a144f35591e959cb90ea1c88c3411e53af3d48e6604b8c3eb297ea418a66`.
Bundle/save identity and all original assets remain unchanged. See UI_AUDIT
for exact local paths and Apple limitations. Never deliver v28/v30 diagnostics.

## Natural Silver and local clan core — v41/v42

The actual earned Silver ID 8 finishes its original three-hour timer after
real closed time. Opening it uses native reward command 213, grants +28 Gold
(119→147), Archers/Arrows/Goblins +1 each, removes only ID 8, retains gems 87,
and clears the journal. Clean profile/cards/home relaunch retains that result.
`/var/tmp/hvm-clash-natural-silver-{open,return1}-v41`; compare the preserved
pre-launch evidence at `/var/tmp/hvm-clash-natural-silver-evidence/before-v41.bin`.
ID 9 remains unstarted, trophies 65, crowns 3/10, King 2/XP 8, ten cards.

v42 qualifies the local solo-clan core in separate **synthetic King-3** fixtures:
original 1000-Gold creation/209, leader/member/info, persistent original chat,
description/type/settings/212, actual local search, leave/208 and paid re-create
with a new monotonic identity. Actual owned SIGKILL at the pre-charge GCL1
journal recovers the same file with exactly one charge. Eight private corruption
cases retain the damaged file at explicit guards, including semantically wrong
commands/records with recomputed checksums. Twelve authored guest tests pass.
See [LOCAL_CLANS.md](LOCAL_CLANS.md) for exact schemas/paths and open controls.
Clans mail, requests/donations and friendly battles remain unported. These
fixtures are not earned King-3 progression, multiplayer, or device evidence.

Private v42 IPA SHA256:
`73e72a742852c4975621bf207eb11a91af8f0d843a76b80d735f3ddb1282b726`;
executable SHA256:
`7e450c931b645dd0cee76bc01daaaf01ec7b18436ee6b66af79305bd9cf7d55c`.
Pinned original assets remain byte-identical; bundle/save identity is stable.
GCL1/GCS1 extend the same atomic checkpoint. Older guests reject these tags;
do not downgrade a clan-bearing instance or discard its save to make it launch.
This internal package is not delivered as the completed offline game.

## Next work

1. Preserve the original phone failure, earned source, actual owned SIGKILL
   evidence and current earned ten-card player. No reset or backup restore.
   Natural Silver collection is now qualified across two clean relaunches;
   battle-result time accounting and native profile Wins still need testing.
2. Finish local clan mail, card requests/donations, friendly battles and the
   remaining controls. Keep original native level/Gold/cooldown rules. The
   qualified solo-clan core is a component milestone, not this feature's end.
3. Then implement authored player-handle variation after training, preserving
   the original training names; compare primary bot/simulator projects with
   pinned native AI before varying legal arena decks and actual decision styles.
4. Restore the original-version daily shop with actual rotation/items/prices
   and atomic persistent purchases; modern shop features are not this client.
5. Extend native arena coverage beyond Arena 1, qualify partial-loss/draw crown
   accounting and earned crown collection, then finish phone visuals/audio/
   stutter and all button scenarios. Keep original reports open until matching
   device evidence passes. Do not invent online players, results or IAP success.
