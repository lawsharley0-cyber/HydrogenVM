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
two earned training victories, chest banking, home persistence, a first native
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

## Next work

1. Finish first reward UI and two relaunch checks, then deliberate journal
   interruption/recovery. Preserve every actual failure and earned save.
2. Continue qualification of deck swaps/eight earned cards and actual level-up
   tower stats; synthetic byte comparisons do not substitute for those scenarios.
3. Qualify NPC2 onward, name flow and all seven earned training wins; persist
   NPC losses, match costs, actual crowns, arena transition and native ELO.
4. Port arena/bot selection and all six locations, real elapsed timers and the
   remaining native economy/journal paths. Build-time data can reuse the pinned
   CSV research, but runtime logic must stay in the guest IPA.
5. Use an ordinary main device build to qualify login/gameplay/audio/visuals.
   Keep the original phone reports open until matching scenarios pass.
