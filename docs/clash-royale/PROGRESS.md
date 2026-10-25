# Offline continuation — October 23, 2026

## Owner report — October 24: defer presentation fixes, continue core systems

The owner reports intermittent/incomplete audio, incorrect King cannon
animation, absent Princess Tower princesses, and text that is not centered in
some UI. These are **user-reported observations**, not yet isolated to a runtime
commit or reproduced against an original-client visual baseline. The target is
the owner's iPhone running client 2.1306.1. The owner explicitly prioritizes
core offline systems and important correctness work before these polish fixes.

Keep these issues in the investigation queue. Do not equate non-silent PCM,
sample-exact codec tests, a rendered tower, or a green shader test with correct
continuous playback, animation or UI alignment. Future reproduction should
record the exact host build, scene/input, audio queue/transport timing, and
matching original-client appearance. Separate missing scene state from
rendering/animation defects before changing either. Do not mask the issues by
removing effects, entities, audio or assertions.

Gemini also changed the local offline backend and chest/progression tools.
The later milestone notes below describe that implementation's reported tests;
they still need independent real-client validation and integration with current
main. In particular, synthetic matchmaking/trophy tests do not prove a complete
arena battle, real rewards, or correct phone playback.

Current owner-directed continuation is `codex/clash-offline-continuation`, based
on main `fdfee6f`. This supersedes the older README/TODO snapshot where it
describes missing host AAC or an unintegrated NPC peer. The owner reports the
current **iPhone** build renders the pinned 2.1306.1 startup at 60 FPS, with
music, then reaches connection retry. That report is not a local FPS measurement.

## Reproduced on the new Linux/WSL build

### Arena entry fees and current Apple build

The bundled arena MatchCost values are now required and read from CSV (Training
Camp 0; arenas 1..6 cost 1..6). Native StartMatchmake 537 does not itself deduct
gold. The service validates/encodes the actual sector first, then atomically
commits its native avatar Gold debit before delivery. FreeGold follows original
commodityCountChangeHelper (0x9a6d4..0x9a728): min(previous FreeGold, new Gold).
Other resource slots/unknown fields are retained. Insufficient/invalid balances
reject without modifying a save. No invented currency or checksum ack is sent.

At native 537 execute entry (0xbbe9c), the current native home is flushed before
the worker prepares the match, including recent upgrades/deck changes. Once a
sector is pending, old home snapshots cannot undo its committed fee; duplicate
pending requests neither charge again nor send another sector. GoHome/login
restore the normal capture path. A started but abandoned battle retains its
entry cost, with no fabricated win/loss/crowns.

Socket regressions compiled against the previous service fail its fee/dedup/
relaunch assertions; the new service passes them. Literal native resource tests
cover fully free and mixed Gold balances, zero training cost and insufficient
funds. `cost-real/run-20261024-223211-27f5f3` accepted an actual Arena 1 sector
and deducted gold 204 to 203. It was deliberately stopped before a result;
this is not another completed battle. Two clean `cost-relaunch` launches retain
203 gold, 87 gems and unchanged battle/crown progress. Runtime 252, Test Lab 14
and the C++ offline state/socket suites pass locally.

Apple checkpoint **a92eb843ab38bdc6a16f1aa7268ee72fdb348470** (before this fee
patch) built an unsigned ARM64 iOS device IPA in
[CI run 40157432144](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40157432144).
The archive/manifest were downloaded and locally reverified; IPA SHA256 is
`e85d0443c4ca9cfbc0dfa18a344643a4ce5d984337a49847c6d33ad368f6a8ae`,
bundle `com.lfull.HydrogenVM`, minimum iOS 26.0. It is unsigned and **not device
tested**. The build includes crown/paid-reward, facing and clock changes, but
not the entry-fee change above. Simulator results are recorded separately when
complete. A successful package check does not verify phone playability, sound
or cannon motion. No game IPA/assets/private saves are bundled in this patch.

### Measured elapsed home time, including interrupted rewards

Complete native home checkpoints now retain a 64-bit `homeSavedAt` timestamp.
Old saves without one start measuring prospectively; no elapsed history is
inferred from account creation. A delivered timestamp is bound to the actual
decoded home using removeLogicClientHome (0x1932c), then to its initialized
LogicGameMode at loadHomeState's continuation (0xd6d40). Original getTick reads
logic+0x38, at 20 Hz. This prevents applying elapsed time to an old UI home or
stamping a paused/loading timer as if it had already advanced.

The original home.fastForward(seconds,-1) API (0x125d7c) applies measured time;
the host clock and serialized timer fields are not edited. Ordinary loads
advance before loadingFinished/first-tick crown transfer. Pending reward
journals defer advancement until their original grant succeeds, then the
grant, elapsed effects and timestamp are saved atomically. Native active ticks
are included so loading/claim pauses are neither lost nor counted twice.
Backward clock movement does not rewind timers. Native seconds-to-ticks input
is bounded at 30 days, beyond the bundled cooldowns, to avoid integer overflow.

Real-time checks use an actual 35-second closed interval and do not modify a
clock or game save. The initial ordinary check measured 62 native seconds over
64 checkpoint seconds. A stricter interrupted-reward check then **failed**:
120 native seconds over 127 seconds. The seven lost seconds led to the native
tick/epoch accounting correction; its tolerance was not relaxed.

After that correction, `elapsed-clock` measured 62.2 native seconds over 62;
actual paid interruption/recovery in `elapsed-clock-paid` measured 79.4 over
79, cleared its 35-byte journal and preserved crown count 1. This was a new
isolated fixture built from an earlier pre-claim checkpoint before fault
injection; no backup was restored after that fault. Earlier fixture preparation
with a missing checkpoint path failed before launching and is not evidence.
The final load-order adjustment was then checked in `elapsed-final`: 65.6
native seconds over 65, crown count 3 and battle counters unchanged, both
launches clean. Full long Silver unlock completion remains unverified.

Runtime 252, Test Lab 14 and C++ state/socket suites pass. Remaining core work:
match-cost transactions, full crown-chest collection/journaling, future victory
label comparison, earned later-arena progression and legacy recovery. Clock
verification does not resolve cannon anchoring, audio continuity, text or
device performance; an Apple build/device run is still needed.

### Original opposing tower directions

Native spawnObject (0x13fe8..0x13ffc) initializes direction Y to +256 for
owner 0 and -256 otherwise. Our full sector replaced both direction components
with zero, making opposite sides choose the same render angle. C++ and legacy
Python writers now retain the original opposing normalized vectors. A literal
packet regression fails before and passes after (three towers per direction).
The real client in `facing-real/run-20261024-214023-a89663` displays the
opposing initial facings and exits cleanly after 111.89 seconds; this short
scene probe was stopped without a battle result. Original assets and native
render-angle logic remain unchanged. Cannon anchoring during motion and iPhone
appearance still require separate validation; initial facing is not that proof.
Runtime 252, Test Lab 14, C++ offline suites and Python protocol checks pass.

### Earned native resource ledger and paid-claim recovery

The native avatar's ChestOrderIndex resource (5000002) was never advanced with
the host's earned chest cycle. Original getNextWinnerChest therefore predicted
Newbie1/Wooden while the host cycle had reached Gold. The saved native index is
now reconciled with the earned cycle, including on each atomic battle commit.
Unknown avatar fields and other resources are retained. Backward/invalid native
indices reject without resetting progress. A literal resource packet test checks
the exact insertion, unchanged tail, idempotence and overflow rejection.
The native prediction and a future victory label still need real-win comparison;
the subsequent real-client checks below ended in losses, not victories.

Arena crown credit now uses original LogicSummoner::getStars, called against
the actual player/opponent summoners at the qualified native result. The
serialized native StarCount resource (5000004) is committed with that result;
the original home.updateStarsFromAvatar transfers and clears it. Historical
crowns are not inferred from wins. Three subsequent actual one-tower losses
advanced native home crown count to 3, retained across two clean relaunches.
This exercises loss credit and persistence, not crown-chest collection at 10.
Native rating decisions, including their small loss deltas, remain unchanged.
Original home encoding has two free chest pointers (+0x78/+0x7c), followed by
crown count/cooldown and the actual crown chest (+0x90); parser names/layout
now reflect that distinction. Newly earned standard chests use native source 1.

`paid-and-crowns/run-20261024-205319-8d1f27` exercises native combined
SpeedUpExploring/claim 506. No 503 follows that command. The service journals
the actual post-cost/unlocked pre-grant checkpoint and original source-1 reward
together. Only the validated current transaction's claim bit is represented as
pre-grant; this is not a repair of arbitrary old half-claimed saves. Actual
gems fell 102 to 87, gold rose 192 to 214, cards were collected and the slot
cleared. The earlier `ledger-real` run stalled at this previously unsupported
combination and exited with its last complete checkpoint preserved.

Actual paid interruption: `paid-fault-v3/run-20261024-211707-c1e7c2` was
intentionally killed after a 35-byte reward journal was saved. Its isolated
save already had gems 72 (down from 87) and pre-grant gold 214. Relaunching the
same save in `paid-fault-recovery/run-20261024-211823-283760`, without restoring
a backup, completed the reward: gold 236, gems still 72, slot removed, journal
cleared. Two 30-second `paid-recovery-relaunch` launches retained identical
derived avatar/deck/collection state and no duplicate grant. An extra recovery
tap started a fixture battle; it was stopped before a result. That is not a
battle-completion result. Earlier fault attempts either had mismatched cloned
credentials or timed out before the button click; neither is fault evidence.

The earned player upgraded Archers and Goblins to level 2 with native cards/
gold, reached player level 2 (0/50 XP), and showed original tower HP 2592/1526.
`ledger-level-win/run-20261024-212046-06bc14` then ended in a loss despite its
experimental directory name. Native score is 61; gold 204, gems 87, three
standard chests and crown count 3 remain. Two `ledger-final-relaunch` launches
exit cleanly and retain those values. Runtime 252, Test Lab 14 and C++ offline
state/socket suites pass after the final parser/ledger changes.

Remaining: native prediction versus visible victory reward, crown chest at
10 and its journal, match-cost transactions, elapsed timers while the process
is closed, earned later-arena progression, legacy recovery and presentation.
Timers currently advance while running; relaunch observations do not establish
wall-clock progress during closure. Do not fast-forward the host clock or save.

### Corrected-combat win, current main and remaining presentation reports

Main `2b60a06` is now integrated. Both sides independently restored the iPhone
108-byte stat ABI; the merge retains the ABI regression check. The mono
AudioFile fix exposed an obsolete TL-D expectation of duplicated stereo data.
The independently derived ADPCM samples remain (0,0,16,48); TL-D now checks
one channel, two bytes/frame, eight returned bytes and an untouched guard.
It failed before that expectation correction and passes afterward. This is
not a weakened codec oracle or a claim that intermittent playback is fixed.

Native echo schemas 500 (two VInts/two packed booleans), 525 (bounded spell
reference list) and 538 (base metadata only) are decoded in full before acting
on a mixed home turn. Native execution/checkpoint capture remains authoritative;
there is no duplicate execution or fabricated checksum acknowledgement. A
mixed echo/518 regression fails before and passes after. Unsupported commands
and malformed/truncated turns remain rejected.

`corrected-combat/run-20261024-152941-0df8fe` produced a real three-crown
Arena 1 win against Trainer Jonas with the restored neutral buffs, original
tower scaling and scenery. Native and metadata trophies rose 34 to 65. The
process returned home and exited cleanly after 206.13 seconds. Two further
30-second clean launches (`corrected-win-relaunch`) retained the win/65 score.
A Silver Chest's original three-hour unlock was started; elapsed completion
and collection of that long timer remain unverified.

The win exposes two concrete remaining accounting defects: the native NPC
victory screen labels its reward Wooden Chest, whereas the host's chest cycle
awarded a Gold Chest; crown progress remains 0/10. LogicBattle::battleEnded
skips setStarCount when battle type is 1 and its NPC reference is present
(0x14ece..0x14ef4). Do not fix this by guessing crowns or suppressing the UI.
Arena bots still use the native training-style NPC battle path; investigate
its proper PvP/native accounting before claiming authentic arena rewards.

Audio capture contains 205.64 seconds of stereo PCM with zero transport drops.
That does not establish continuity: analysis of seconds 30..120 finds a longest
exact-silence run of about 1.03 seconds and 0.283% clipped samples. The original
battle music decoded independently by ffmpeg contains no exact silence in that
same source interval. Scene/track timing differs, so this is a lead for tracing
audio transitions, not proof of an underrun or a speaker/device diagnosis.

The owner clarified that visible princesses face the wrong direction and the
King cannon does not stay anchored. These remain unresolved orientation/local
transform defects, alongside intermittent sound and text alignment. Restored
visibility is not evidence of correct transforms. Compare native animation
state and period footage, without substituting modern game assets.

Latest gate: runtime 252/252, Test Lab 14/14, C++ offline suites, plus the full
Linux compatibility harness: zero unit/Python failures, four fixtures run,
zero crashes/skips and zero baseline regressions. The first harness attempt
lacked built asm/C guests and the expected `ipas/` fixture directory and failed;
it is not pass evidence. The subsequent `main-integration-compat-ready` run
used rebuilt guests and hash-qualified fixtures. Short fixture smokes still do
not resolve the owner's iPhone app reports or establish Clash progression.

### Native free chest, deck swap and all six arena scenes

CollectFreeChest is command 518, whose decoder has only base command metadata.
It now requests the original native randomizer/213 encoder with source 2. The
durable journal distinguishes standard-slot and free rewards, retains the
pre-claim home, and clears only after the native reward execute succeeds and
the resulting home is captured. Older format-4 standard journals remain readable.
No timer-only home change can prematurely acknowledge a pending free reward.

`free-fixed/run-20261024-150207-6c299f` exited cleanly after 478.71 seconds.
The original reward gave 22 gold, 2 gems and Goblins (gold 170 to 192, gems
100 to 102). Goblins were then swapped into the deck in place of Arrows using
the original UI. Two clean 30-second launches in `free-player-persistence`
retained identical derived deck, collection, chest and avatar state.

An isolated pre-claim fixture was killed only after its actual 46-byte source-2
reward was durably journaled (`free-fault-v2/run-20261024-151443-97c5a0`,
intentional SIGKILL, not a spontaneous crash). `free-fault-recovery/run-20261024-151630-43d188`
replayed that journal from the same save without restoring a backup, completed
the native reward, and exited cleanly after 124.11 seconds. Two further clean
30-second launches retained the collection/deck and cleared journal, with no
duplicate grant. Native avatar bytes changed during those later launches;
do not describe the entire native checkpoint as byte-identical. The first
interruption attempt used an already-claimed fixture and timed out without a
kill; it is not recovery evidence. Journal transport unit tests do not validate
reward contents; the actual client runs above do.

`arena-layout-fixtures-v4/evidence-1` through `evidence-6` each loaded a native
battle scene and an attacking native NPC, with clean approximately 90-second
exits and no reported errors. This covers all six bundled arena locations,
including their original decorations and tower occupants. These are explicitly
synthetic arena/trophy fixtures isolated from the earned player: they do not
prove earned promotions, later-arena wins, rewards or balance. Earlier 45-second
attempts stopped at home/notices and do not count as battle-scene passes.

After the free-chest changes: runtime 247/247, Test Lab 14/14, and C++ offline
socket/state suites pass. Pending core work includes arena match costs, crown
accounting, long-duration Silver unlocks, corrected-combat victories and earned
later-arena progression. Client echoes 500/525/538 still need schema-qualified
handling; their rejection is logged, never a blanket successful acknowledgement.

### Tower initialization, shrinking health bars, occupants and original scenery

Corrected native tower level/HP serialization using exp_levels and the pinned
global growth percentages (King 8%, Princess 9%). Original getScaledValue uses
an integer accumulated percentage; NPC WizardHpMultiplier applies once to both
tower types with ceiling division. An old King-only adjustment was removed to
avoid applying it twice. NPC8 now displays level 2 and 1526-HP Princess Towers
(King 2592); the synthetic tests also check one 40% multiplier on each NPC tower.
`scaling-real/run-20261024-141236-938b3e` completed a deliberate unplayed loss,
saved the native loss count, and exited cleanly after 322.52 seconds. Native
score remained 34: no invented loss deduction was applied.

The zero-filled buff fixture was a real core/combat error. Constructor
LogicCharacterBuffComponent C2 (0xf10c, especially 0xf164..0xf184) sets +0x34
(damage percentage) and +0x44 (size percentage) to 100. Supplying zero makes
calculateSize (0xff40) clamp to 1%. The original getMaxHitpoints (0x21428)
then computes only 1% of base HP. EntryTrace showed current/max 1526/15 and
an invalid negative animation index. The client was not rendering a correct
state; replacing the health-bar drawing would have hidden the real bug.

Both C++ and legacy Python sector writers now encode neutral percentages.
A literal packet regression fails before and passes after. All four Princess
Tower occupants render again. `health-fixed` trace shows size 100, current/max
1363/1526, 99 total frame intervals, quotient 88 and therefore native fill frame
11. The bar visibly shrinks; no damage, health or animation assertion is hidden.
King cannon motion still needs an exact-scene reference/device check. These
initialization fixes change tower combat; earlier victories establish the old
progression path, not correct combat balance in the corrected state.

Trees really are in this build. LogicBattle::addDecos (0x15a44) creates the
original CSV decorations before decoding the full sector. Our six-tower-only
reference list caused LogicGameObjectManager::decode to delete those omitted
objects. The packet now retains data type 42 references, native type-0 object
IDs 1000000+i and its ID counter in original row order. LogicDeco::decode is
empty, so native row positions are retained. No assets or scenery coordinates
are changed. `deco-real/run-20261024-143701-91a907` now visibly includes trees,
rocks, fences and arena-edge scenery, with tower occupants; it exited cleanly
after 305.4 seconds. A synthetic tree regression checks retained identities.

Runtime 247, Test Lab 14, C++ state/socket checks and Python protocol/home/chest
tests pass locally. These are Linux/WSL observations. Audible continuity,
text alignment, all-arena gameplay and newly signed phone validation remain
open; PCM activity is not proof of audio quality.

Reference correction: the first-look video linked below is uploaded by verified
Chief Pat, not Supercell's own channel. Browser inspection reached its Royal
Arena footage at about 2:23. It is a contemporary creator gameplay reference,
not verified exact-2.1306.1 footage or a Goblin Stadium pixel oracle. The pinned
CSV/resources and original native initialization are the authoritative source
for the restored objects in the local build.

### Native rating calculation and additional owner observations

The owner now explicitly authorizes continuing through core progression into
the full bug-fix phase. Newly reported: health bars do not shrink when damaged;
Princess Tower occupants are absent; surrounding trees may be missing. Preserve
these alongside intermittent sound, King cannon movement and text alignment.
They are not resolved by a native win or a passing unit suite.

The trophy blocker below was reproduced and corrected at its source. Native
LogicGameMode::updateScores (0xd85c4) skips rating changes when either avatar's
arena equals the starting Training Camp. Our arena-sector bot was incorrectly
encoded in that arena with zero trophies and level 1. The sector now supplies
the active arena and original NPC row's Trophies/ExpLevel. NPC8 is 50 trophies,
level 2. The observer reads the native score after updateScores instead of
adding experimental +30/-20 deltas; the obsolete fallback live-score write is
removed. On load, metadata is reconciled to the persisted native score/arena,
preserving the native avatar and all other progress, so the old metadata-only
30 is not treated as an earned native score.

`native-elo/run-20261024-133357-e85349` completed another Arena 1 win against
Trainer Jonas, this time 2 crowns to 1 at time limit. Native score became 34,
the original home displayed 34, and metadata also stored 34. The run exited
cleanly after 339.67 seconds. Two further clean 30-second launches in
`elo-relaunch` retained native and metadata score 34. Observer-only tests now
explicitly supply scores 34 and 3 and test capture, not a made-up rating
formula; both fail against the previous fixed-delta implementation and pass
after. The pinned NPC8 fixture checks original score/arena/level. Runtime 247
and Test Lab 14 still pass. Native bot/tower level scaling, cost/crown/free-chest
accounting and higher arenas remain unverified; this is not completion of all
offline systems or the reported presentation bugs.

Reference work: the pinned `res/locations/goblin_arena.csv` contains
`goblin_tree1` through `goblin_tree4` placements. Thus tree definitions really
exist in this build; whether each should be visible depends on camera/viewport
and client decoration loading. Do not add modern trees or edit assets by guess.
The buildings table references PrincessB on PrincessTower. Official contemporary
footage is [Gameplay First Look](https://www.youtube.com/watch?v=x6aQ67jk58I),
embedded in the [January 4 launch report](https://toucharcade.com/2016/01/04/supercell-clash-royale-soft-launch-moba-ccg/).
Its exact executable version has not been verified; it is a period reference,
not a pixel-perfect oracle for 2.1306.1. Video fetching was throttled. Use the
pinned CSV, original symbols and a verified reference frame before claiming
visual parity. Native Character::updateHealthBar computes a frame index from
current/max HP at 0x9f03a..0x9f056; investigate the resulting movie-clip state
and rendering rather than replacing the damage logic.

### October 24 training graduation and name-dialog blockers

**Latest core result:** the graduated player also completed a native three-crown
Arena 1 win against NPC8/Trainer Jonas. `arena-one-fixed/run-20261024-132350-b0cf84`
exited cleanly after 241.19 seconds. The preceding `arena-one` run stopped at
the client's genuine "Full update stream is corrupted!" diagnostic: the
inherited non-tutorial sector writer replaced required marker 12 with 0.
Native LogicGameMode::decode at 0xd7a80 compares that field to 12 regardless of
tutorial presence. The writer now retains 12 and clears only the tutorial
reference. A pinned-client fixture test fails before and passes after; the
actual Arena 1 battle then loads/deploys/finishes normally.

**Still blocking faithful arena progression:** after this eighth win metadata
has 30 trophies but the native serialized avatar still has 0. Existing +30/-20
formulas and CSV routing are experimental; their synthetic tests do not prove
native score synchronization. Next work must deliver and persist a legitimate
native/server score update, verify battle cost/rewards/crown accounting and
relaunch, then validate levels and more arenas. Do not fix this by silently
writing live guest trophy fields or count the metadata as earned UI trophies.

Saved-state inspection also caught an error in checkpoint `33d82ad`'s new name
command: its change-state uses a fixed BE int (decoder slot 0x48), whereas the
avatar snapshot uses VInt. The initial one-byte encoding happened to close the
dialog but produced state 0x1007f. The command and literal oracle are corrected.
On login only that exact malformed state is repaired by replaying command 201
with the same existing name and proper integer width; no private save is edited
and paid renames remain unsupported. Verify the native saved state is 0, not
merely that the dialog closes. No other name states are reset.
`name-int-recovery/run-20261024-132831-49e9ac` verified this on the actual
eight-win saved player: the first launch reapplied the native command, saved
nameState 0 with the same name, and exited cleanly; the second launch exited
cleanly without another repair. Wins remained 8 and arena remained 54000001.
The final build again passes 247 runtime checks and all 14 Test Lab cases.

The same earned player reached all seven training wins without editing the
IPA/assets or saved counters. `training-resume/run-20261024-130720-a6dbd1`
won against NPC6/Trainer Tim and atomically saved 6/7, then stopped with exit
70 at missing CTLineGetOffsetForStringIndex while rendering the native name
dialog. The win and both separate chests survived that failure. This is a real
runtime blocker, not an intentional fault test. The earlier `training-finish`
process ended with its WSL host and has no completed summary; its final frame
was an unfinished NPC6 battle. Do not count that run as a clean pass or win.

The shared CoreText API now returns actual laid-out glyph positions in source
string coordinates and writes the optional 32-bit CGFloat secondary offset.
Apple's documented single-caret behavior is the reference:
[CTLineGetOffsetForStringIndex](https://developer.apple.com/documentation/coretext/ctlinegetoffsetforstringindex(_:_:_:)).
TL-N uses our CC0 font and hand-derived 600/1000-em advances: a substring at
size 20 has offsets 0/12/24, equal primary/secondary caret, and empty-line 0.
It fails before and passes after. Existing complex shaping limitations remain.

The next normal client request is ChangeAvatarNameMessage 10212 (BE String and
user flag, decoder 0x7ce7c). The backend supplies native command 201 in 24111:
name, state, user flag, server ID and base command metadata, derived from
0x122958/0x130f18/0x639a0. The original command sets the name and notifies the
dialog; native snapshots persist it. A failure before snapshot simply offers
the initial dialog again. Already-user-named avatars are rejected because paid
renaming is not implemented. No direct live-avatar write or fake turn ack.

`training-graduation/run-20261024-131511-b3002b` then completed the native name
dialog, collected both remaining chests (gold 144 -> 157 -> 170), and defeated
NPC7/Trainer Boris by three crowns in TrainingCamp. Native home transitioned
to Goblin Stadium / Arena 1 with 0 trophies and a new 3-hour Arena 1 chest.
The native seven-win avatar and reward committed together. The run exited
cleanly after 268.74 seconds; 11,601,066 non-silent audio frames, no transport
errors/dropped blocks. This does not resolve the owner's audible glitches.
Two further 30-second launches in
`graduation-relaunch/run-20261024-131950-487d23` exited cleanly with no errors.
The derived avatar SHA/deck/chests/counter summary was byte-identical before
and after those launches; the Arena 1 home remained accessible.

Validation: 247 runtime checks, 14 Test Lab cases, C++ offline/state tests,
26 Python protocol/home tests, and 8-second Angry Birds/Tiny Tower/Art of Glow
smokes. Short Linux smokes do not resolve the owner's device crash reports.
Next core work is a real Arena 1 battle/reward and correct trophy transaction,
NPC/tower level scaling, collection/deck swaps and later arenas. The older
Gemini milestone below describes experimental formulas and synthetic coverage;
it is not proof of authentic arena progression. Overall bugs stay deferred.

### October 24 functionality pass: upgrades, later NPCs and interrupted collection

The owner explicitly defers overall bug/performance/presentation fixes to the
final stage. This pass stays on core progression. Main `462ca72` was integrated
without replacing unrelated work. The merged runtime passes 246 checks and all
13 Test Lab cases; this remains Linux/WSL evidence, not a new iPhone build.

The native Knight upgrade completed through command 504: 116 -> 111 gold,
0 -> 4 XP, displayed level 1 -> 2, two required cards consumed, and original
stats updated (715 HP). Its native deck/avatar snapshot survived relaunch
unchanged. Bomber and Arrows later upgraded through the same original UI to
level 2, each costing 5 gold and granting 4 XP. No upgrade prices, stats, game
code or assets were patched. Rewarded Musketeer and Mini P.E.K.K.A. were added
by the native client, filling all eight deck slots.

Native three-crown wins were observed against NPC2/Trainer Fikova, NPC3/Trainer
Mike, NPC4/Trainer Klaus, and NPC5/Trainer Teemu. Home progressed through 5/7,
with original timer/double-elixir/NPC deployments and upgraded player cards.
Key runs: `progression/run-20261024-015138-cab068` (upgrade, 217.78 seconds,
clean); `npc2/run-20261024-015959-ba375d` (359.11 seconds, clean);
`npc3/run-20261024-020604-b70318` (win then intentional fault test, below);
`npc4/run-20261024-021819-a73fad` (644.09 seconds, clean);
`training-finish/run-20261024-022857-1ede4f` (subsequent training, see later notes).

Two new core fixes:

- Native addChest at 0x124bf0 increments home+0xbc and stores the result in
  chest+8. Our injected rewards had all used ID 0. Added rewards now advance
  that serialized counter and preserve existing IDs/timers. Commands 502/503/506
  use a chest ID, not a 0..3 slot index. Tests fail before and pass after for
  distinct identities, slot reuse, larger IDs and a VInt counter-width change
  preserving the native avatar boundary. The real client held two chests with
  IDs 1 and 2, unlocked them independently, and collected ID 2 without touching
  ID 1. The first chest's countdown also completed during an NPC battle.
- Real win/loss metadata is held until the post-setter native avatar capture
  commits counters, reward and updated avatar together, preventing a new win
  from being persisted alongside an older avatar. NPC3 and later exercise this
  path. Also corrected the seventh training mission: the pinned Npc7 row says
  TrainingCamp; the handler must not force Arena 1 before graduation. A socket
  regression fails before and passes after this routing change.

Real interruption test: while collecting the second chest, an external watcher
identified the exact owned test process by its report-path argument and sent
SIGKILL after a 35-byte native reward was durably journaled (serial 1, slot 2
in the on-disk one-based field). This is why the NPC3 probe reports exit -9 and
`crashed=true`; it is deliberate fault injection, not a spontaneous runtime
crash. A private backup was taken but was **not restored**. Relaunch loaded the
actual journal, delivered its original reward, increased gold 111 -> 124,
removed only the second chest and cleared the journal. The first chest (ID 1)
remained unlocked and unclaimed. `reward-recovery/run-20261024-021438-b32519`
then exited cleanly after 96.39 seconds. Two further 30-second launches in
`recovery-stability/run-20261024-021611-5b0aec` retained identical deck,
avatar, chest and reward state, with no repeated grant. Collecting ID 1 later
granted its original 16 gold and unlocked Musketeer.

Code checkpoint: `d514e7d`. The diagnostic probe now recognizes later native
NPC IDs rather than matching only the literal Npc1 log. Historical NPC2/NPC3
summary booleans predate that diagnostic correction; their native logs and
observed victories provide the actual mission evidence. Older half-claimed
saves without a journal, legacy duplicate-ID saves, full arena progression and
device validation remain distinct work. Deferred sound/animation/princess/text
issues remain open; clean audio transport is not proof they are resolved.

### October 24 next phase: complete earned-chest transaction and persistence

Merged current main `93d05af` without conflicts, preserving local work. The
merged baseline initially passed 244 runtime checks and 13 Test Lab cases but
the real client regressed at font loading. A native trace found an opened font
with length zero: plain `_fstat` had been changed to the legacy Mac layout.
Apple XNU [cdefs.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-3789.1.32/bsd/sys/cdefs.h)
sets `__DARWIN_ONLY_64_BIT_INO_T=1` for iPhoneOS and omits the `$INODE64` suffix;
[stat.h](https://github.com/apple-oss-distributions/xnu/blob/xnu-3789.1.32/bsd/sys/stat.h)
defines the corresponding structure. ARMv7 uses a 108-byte structure with size
at offset 60. Restored that ABI for plain stat/lstat/fstat, removed unused legacy
writers, and corrected the earlier unit oracle that assumed the Mac layout.
New TL-H v2 writes a known 1024-byte file, reads raw fstat bytes and verifies
the 108-byte boundary. It fails before and passes after; no font/URL workaround
or assertion suppression was needed. Survivalcraft's reported issues still need
their own real-app investigation, not a return to the wrong iPhone ABI.

Fresh native run `reward-fixed-stat/run-20261024-012658-945b2c` completed a
three-crown win, 414.09 seconds, clean exit. It exposed an additional accounting
gap: no battle log was generated, so observing only the log's reward getter
missed the earned chest. NPC reward banking now follows the *bundled chest
order* after the qualified original win counter transition, exactly once. This
is the offline backend's reward policy, not proof of the historical server's
implementation. One recorded but unbanked native win can finish that transaction
on restart; GoHome and a result-screen preview alone still cannot award a win.
Post-setter continuations also preserve the original avatar after the native
counter/arena update, before GoHome can substitute an earlier home snapshot.
This is needed for subsequent wins; NPC 2+ still needs real-client validation.

The original LogicRewardRandomizer exists at 0x132f98. The adapter mirrors the
original LogicProgressSimulator::openChest construction sequence at
0x8f2b4..0x8f318, creating temporary native objects, calling the original reward
randomizer, and using the original command encoder 0x6d8c4. No manual drop
formula, live-guest field overwrite or game-byte patch supplies the reward.
The peer sends AvailableServerCommandMessage 24111 containing native command
213. A format-4 save journals its immutable bytes, source slot and serial against
the complete pre-claim home *before* delivery. A session sends each serial once;
a reconnect replays it against the pre-claim checkpoint. Only a completed native
collection (claim flag cleared and source chest removed) commits the new home
and avatar and clears the journal. Older half-claimed saves without a journal
are not silently reset or re-randomized. The snapshot flag previously named
`coOpen` is now correctly named `claimed`.

Real run `reward-collect/run-20261024-013540-ee0032` unlocked Newbie1 for the
original 15 seconds, opened and collected it through the original animation.
Gold changed 100 -> 116; Knight and Arrows counts changed 1 -> 2; all chest
slots became empty and the reward journal cleared. Runtime 207.66 seconds,
exit 0, no crash/errors; 9,132,032 audio frames, 8,830,321 non-silent, zero
transport errors or dropped blocks. Sound continuity on iPhone remains unverified.

`reward-relaunch/run-20261024-013900-3c552c` then ran two 45-second launches,
both clean. Home retained 116 gold and 1/7 training, cards retained their earned
counts, chest stayed removed, journal stayed empty, and native avatar SHA-256
was unchanged. Socket tests independently check journal replay after two backend
restarts, once-per-session delivery/heartbeat alignment and corrupt-journal
rejection. These tests do not yet cover interrupting the real app mid-collection.

Post-integration checks: 244 runtime checks, 13 Test Lab cases, 26 Python Clash
tests, C++ socket/state/observer suites. Angry Birds, Art of Glow and Tiny Tower
each completed an 8-second startup smoke without reported crashes; this does not
resolve the owner's device reports. New limits to verify next: card upgrades,
NPC 2 onward, multi-chest IDs/slots, real interrupted-claim recovery, graduation
and arena rules. Apple compilation/device tests and deferred presentation defects
remain open. All game captures, logs, saves and commercial assets stay outside Git.

### October 24: command validation and incomplete chest transaction

Reviewed Gemini's local changes without replacing them. Native getters and
decode functions establish StartExploring=502 (0x10f7c8/0x10f7ec),
StartRewardClaim=503 (0x73540/0x73564), FuseSpells=504 (0x18f08/0x18f28),
SpeedUpExploring=506 (0x17a40/0x17a6c), StartMatchmake=537
(0xbbf20/0xbbf4c). Matchmake includes a VInt and packed boolean after the common
four-VInt command base; it is not complete at the command ID.

The old handler started a battle from a truncated matchmaking message. A new
encrypted socket test fails before the fix and passes after: complete bounded
turn parsing now precedes dispatch, verifies the account and -1 debug trailer,
and rejects unknown/truncated commands without replying or losing RC4 alignment.
Literal decoder-derived vectors cover chest commands, wrong IDs/executors,
invalid references, truncation at every byte and trailing data.

Real-client run `core-oct24/run-20261024-001903-e7f70a` lasted 227.78 seconds,
exit 0, no crash/errors. Original home shows 1/7 and earned chest OPEN.
Tapping the chest logs `Simulation of command 503 failed, error code 4`.
Native execute at 0x7357c returns 4 when chest+0x18 is already claimed; that flag
was previously mislabeled `coOpen` in our snapshot view. Native setClaimed
(0xf1a60) sets it and marks the home `isClaimingReward` at +0xb4. This is not
completed collection: the separate server-side LogicClaimRewardCommand is 213
(0x8283c). Reward delivery, inventory/wallet changes and atomic completion are
the next blocker. Do not reset the claimed bit or mint substitute rewards to
conceal this failure.

Checkpoint capture now waits while the original home isClaimingReward getter
(0x12580c) is true, retaining the previous complete checkpoint. A focused
observer test fails before this guard and passes after. Older half-claimed saves
remain an explicit recovery task; the guard does not repair them. Gemini's
fixed trophy formulas and guest trophy writes remain experimental and require
native/server-contract review before claiming faithful arena progression.

Post-change Linux checks: 234 runtime checks and 12 Test Lab cases pass;
26 Python Clash tests and the C++ state/socket suites pass. The captured real
run delivered 10,016,768 audio frames, 9,185,101 non-silent, zero dropped blocks
or transport errors. This is not verification of continuous sound on iPhone.
Local full logs, screenshots and private saves remain outside Git under
`/var/tmp/hvm-offline-20261023/evidence/`.

- Unmodified pinned IPA, same SHA-256 as the README; shared GPU backend enabled.
- 234 runtime checks pass. Baseline Test Lab: 12 passed, none failed/skipped.
- 18 Python Clash tests pass, including a real encrypted TCP session that sends
  an unknown turn command and then successfully exchanges a heartbeat. Unknown
  diagnostic metadata is recorded separately, never acknowledged as executed.
- Restored native VInts and explicit local NPC sector integration reach the
  original arena, speech, cards/elixir, troop deployment and combat.
- **A three-crown native victory over Trainer Earl was observed**, with the
  original "Winner! You" / Wooden Chest result screen, victory music and crown
  effects. No guest code/assets/settings were modified and no turns were echoed.
- Native trace at `0x12b4e` returns finish requirement 5 for both Knight and
  Archer commits. Both tutorial cast steps complete; this is no longer the
  old first-cast blocker. The star-gained step then completes through combat.
- NPC Goblins deploy and attack; both sides destroy towers. This is native AI,
  not Python battle simulation.
- Pressing result OK sends **14101 GoHomeMessage, version 0, one-byte body**.
  Its native encoder `0xea960` writes a boolean at message+0x30. This is not an
  outcome/reward payload. The current peer lacks the return-home/progression
  implementation, so it must not award a win merely because 14101 arrived.

Local-only evidence root: `/var/tmp/hvm-offline-20261023/evidence/`.
Initial run: `battle/run-20261023-193254-1fd600` (217.56 s, clean exit,
no errors/crash, 9,566,208 mixer frames / 7,741,594 non-silent, zero audio drops
or transport errors). Aggregate 4,451 frames over 217.155 s, p50 frame 33.29 ms,
p95 50.86 ms; includes loading and inspection. This is not the reported phone
60 FPS result or a matched before/after benchmark.
Victory trace run: `trace/run-20261023-193637-402ac1`; uses existing
`HVM_TRACE_BLOCKS=12978-12ba0`, which single-steps blocks and is deliberately
slow. Its FPS must not be used as a production performance measurement.
Screenshots, full traces, tokens, assets and saves remain outside Git.

## Current gaps

The Python peer is a desktop validation harness. **It is not an iPhone offline
service.** The phone needs an in-process service with app-session lifecycle,
persistent native outcome/reward state and local routing. Current iPhone retry
is not fixed by shipping these Python flags alone.

Earned progress, chest granting/opening, return to home, subsequent NPCs and
arenas are not verified. Do not equate the visual reward preview with an earned
saved chest. A native outcome observation or independent native simulation is
needed; blind 14102-to-21902 replay remains prohibited.

Combat exposed a missing codec: `demon_step_01.wav` is mono 22050 Hz Microsoft
ADPCM (WAVE format 2), distinct from the existing 107-file IMA WAV corpus.
The runtime reports that sound as unsupported. Keep it in scope for a shared
codec fix with an independent synthetic vector and real-asset verification.

Only six tower objects are encoded in the initial sector. Decorative-map and
full UI fidelity still need comparison with the original location data. Good
startup artwork and a rendered arena do not establish complete visual parity.

## Reproduction

```sh
python3 -m unittest discover -s tools -p 'test_clash_*.py'
python3 tools/clash_home_probe.py --hvm /path/to/current/hvm \
  --ipa /path/to/pinned.ipa --out /tmp/clash-player --results /tmp/clash-runs \
  --seconds 1800 --launches 1 --capture-audio --npc-fixture --live-controls --gpu
```

Advance both original speech pages with taps around (210,340) in points.
The first Knight is at (100,500); drag through (100,400) to (100,275).
The Archer is at (160,500); drag to (100,275) promptly to support the Knight.
These screen points produce native target (5500,18500); use the visible target
and inspect the native command rather than guessing from framebuffer pixels.
Subsequent Giant/Knight deployments used the left lane. A Fireball on the enemy
King around (160,65) finished the observed battle. Original troop and tower
health/damage were retained. Delaying between deployments can lose troops.

Touches are append-only lines: `touch 0 x y` begin, `touch 1 x y` move,
`touch 3 x y` end; `stop` finalizes the probe. The runner reports point scale 2
for the 640x1136 frame. Do not replace the binary while the probe hashes it.

Interactive experimental wrapper:
`python3 tools/clash/clash_desktop.py --hvm ... --ipa ... --home ... --out ... --gpu`.
Its actual window/speaker path still needs a smoke test; headless mixer capture
does not prove speaker audibility.

## Reference research

[Scroll](https://git.xeondev.com/Supercell/Scroll), reviewed at
`d897141dd5fb6c09753ff29761ef95c051c4655f`, reuses retained native LogicGameMode
for a server on a different 1.3.2 client. Its session handler schedules commands
by execution tick, ticks native logic and compares calculated checksums. It is
an architectural reference, not a wire-layout oracle for our 2.1306.1 executable.
Do not copy its addresses or initial state into this client.

[royaleoffline](https://github.com/natesworks/royaleoffline), reviewed at
`39da88c20896edff7dade7a4ee2b39771b067d45`, is another research reference;
no code or game assets have been copied from it. The earlier Magich03/CR-1.2.3
opaque sector remains unsuitable as a pinned-client fixture.

## Progression & Chest Cycle Milestone — October 23, 2026

- Implemented `ClashCsvEngine` in C++ (`src/runtime/offline/ClashCsv.hpp`, `ClashCsv.cpp`)
  and Python (`tools/clash_chests.py`).
- Resolved Supercell CSV inheritance across `treasure_chests.csv` (`BaseChest`) and
  handled multi-line continuation records in `chest_order.csv`.
- Mapped all 6 Newbie chests and the 240-chest `MainCycle` rotation with live arena
  reward multipliers (`arenas.csv`).
- Verified all 7 Training Camp missions (`Npc1` through `Npc7`: Trainer Earl, Trainer
  Fikova, Trainer Mike, Trainer Klaus, Trainer Teemu, Trainer Tim, Trainer Boris)
  against the unmodified app bundle.
- Integrated `chestCycleIndex` into `ClashProfile` and atomic Plist save persistence.
- Verified test suite: all 24 Python unit tests and all C++ test runners pass.

## Arena 1, Trophy Progression & PvP Milestone — October 24, 2026

- **Trophy Progression & Arena Promotion/Demotion:**
  - Added `trophies` and `arenaLevel` tracking to `ClashProfile` and atomic Plist serialization.
  - Implemented authentic promotion/demotion logic in `ClashCsvEngine::getArenaForTrophies()` using `arenas.csv` `TrophyLimit` and `DemoteTrophyLimit` (Arena 1 -> Arena 2 at 400 trophies, demote at 350, floor at Arena 1).
  - On Arena match victory: player receives +30 trophies and arena-scaled victory chests.
  - On Arena match defeat: player loses 20 trophies (bounded by 0 floor).
  - Synchronized guest avatar score at `avatar+0x34` during reward commits so in-memory native avatar serialization reflects updated trophy counts.
- **Matchmaking & Bot Deck Engine:**
  - Parameterized `ClashFixtures` to support all 20 NPCs (`Npc1` through `Npc20`) and full level ranges (ExpLevel 1-13).
  - Resolved dynamic map location lookup from `arenas.csv` `PvpLocation` (Goblin Stadium `locations/goblin_arena.csv`, Bone Pit `locations/bone_arena.csv`, Barbarian Bowl `locations/barbarian_arena.csv`).
  - Added `findBestNpcForTrophies()` to match the player against appropriate bot trainers (`Npc8` through `Npc20`) based on live trophy count.
  - Turn command 537 (`LogicStartMatchmakeCommand`) automatically loads the appropriate PvP arena sector and bot opponent.
  - Enabled non-tutorial sector mode for Arena 1+ (suppresses training finger guides).
- **Turn Command Decoding:**
  - Added command dispatching in `14102 EndClientTurnMessage` for 503 (`LogicStartExploringCommand`), 504 (`LogicFuseSpellsCommand`), and 505 (`LogicSpeedUpExploringCommand`).
- **Comprehensive Verification:**
  - `hvm_clash_state_tests`: verified VInts, CSV tables, chest inheritance, cycle rotation, drop calculations, arena thresholds, NPC matchmaking, and all 7 Training Camp NPCs plus Npc8 Arena 1 Goblin Stadium loading.
  - `hvm_clash_tests`: 14/14 tests pass, including login, sector delivery, matchmaking turn command, training camp graduation to Arena 1, +30 trophy win awards, -20 trophy defeat deductions, and relaunch persistence.
  - `test_clash_*.py`: 26/26 tests pass in 1.08s.
