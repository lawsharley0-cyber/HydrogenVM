# Native donation work — October 26, 2026

Latest post-127ad31 continuation: [native clan controls](CLAN_CONTROLS.md)
qualifies Promote/Demote/leadership/Kick and persistent removal, with GDN1-v2
and a required-child flag in GCL1. Twenty-five tests, sixteen unchanged-file
negative cases and four pre/post-commit abort cases (two returns each) pass.
Reciprocal donations/supply/invitations/weekly stats remain open. The v1 donor
format and the role/kick blockers described below are the preserved earlier
stage; do not downgrade a migrated instance.

Status: the post-0be234d continuation adds a persistent **Local Donor** member
and actual native NPC-to-player common/rare transfers. Its finite native starter
inventory, the human inventory and request state commit atomically. This is a
qualified subset: player-to-NPC donations, stock replenishment, role/kick
controls, weekly statistics and broader clan interactions are still unfinished.
It is not the completed clans feature or a fresh iPhone qualification.

This follows Kimi's `96003cb` friendly lifecycle and `5666195`'s independently
reproduced post-friendly upgrade/relaunch and stream-ID fixes. The guest owns
its offline transport and save; no title backend runs in HydrogenVM. Shared
emulator `src/` is unchanged in this donation investigation.

## Persistent member and live transfer qualification

The default guest now seeds one explicitly simulated Local Donor (3,1) once
when the player owns a completed local clan. Existing clans migrate only after
all native/extension validation. Original **JoinAllianceCommand 209** with
`create=false` joins the NPC as role 1 without charging the human; the player
remains leader. The NPC is explicitly seeded King 3, zero trophies, original
100 Gold/100 gems and the pinned six-card starter deck, one spare material per
card. These are authored NPC starting conditions, not earned human progress.
No material is replenished on relaunch. The roster shows 2 members, online 1/2
(the actual running human); native profile (14113→24113) uses the persisted NPC
home/avatar. The native name-set flag is necessary to avoid displaying Unnamed.

GDN1 is inside the same atomic CRG1 file, not a sidecar. Its **nine LE u32**
words are tag, version 1, clan serial, NPC body size, native avatar offset,
accounted wall epoch, human-body FNV, NPC-body FNV, and header FNV over 32 bytes.
The original serialized NPC home/avatar follows the 36-byte header. Check all
identities, name, role/membership, bounds and canonical native roundtrip before
any refresh/migration write. A fulfilled GCR1 must have this donor's consumed
stock and actual native donation history. Pending clan settings update the
NPC's badge atomically with the native human change; completed leave/disband
clears the NPC/request in the same commit. Older guests reject GDN1: **do not
downgrade an instance**. Earlier private member diagnostics used a provisional
32-byte layout; those private copies are not release saves or upgrade fixtures.

`donation_tick` runs on the original guest home/update thread after a settled
request, not in a host backend or concurrent timer. It requires the actual
completed clan, owned eligible common/rare card, native arena capacity and
per-donor limit, remaining NPC material and native donation cooldown/capacity.
The current conservative request window is 5 seconds through less than 8 hours;
this is an implemented bound, not a claim that a historical stream entry must
disappear at exactly 8 hours. Native NPC elapsed time is measured from GDN1 and
advanced with the original home time API, without refilling stock or collecting
chests. Active rewards/friendly challenges and battle/transition UIs defer it.

Both **203 and 211 execute synchronously** through original native functions,
including the live receiver's original listeners/toast. No deferred server
command is queued, so a separate pending receiver journal would introduce an
unnecessary replay risk. The detached NPC must consume exactly one material
and receive its original Gold/XP before the player receives anything. Actual
receiver material/wallet deltas are checked. Both post-states plus the original
DonationContainer and GCR1 stream entry commit via one existing fsync/rename.
Only after that commit is the updated 24312 stream delivered. Pre-rename
interruption retains both old states; post-rename restart sees the donor entry
and never repeats 203/211. The NPC's zero-stock failure cannot credit a player.

Actual pinned-client evidence on the unchanged qualified 912a87e Linux runtime:

- `/var/tmp/hvm-clash-donation-atomic-report`: original local clan/profile,
  Arrows request becomes **1/10**, human material 1→2, NPC 1→0, NPC Gold100→105
  / XP0→1. Human Gold2940/gems87/XP6, trophies65 and NPC counters9/2 retain
  their values. `/var/tmp/hvm-clash-donation-atomic-return1` repeats this exact
  saved instance without another transfer; the NPC's native profile reports 1.
- `/var/tmp/hvm-clash-donation-rare-send-report`: original picker states
  **10 common or 1 rare**. A real command524 replaces the old request and
  requests Giant; native receipt toast appears and the bar becomes **1/1**.
  Human Giant material0→1, NPC1→0, NPC Gold105→155 / XP1→6. This separate
  component copy explicitly ages human/NPC accounted stamps by 28805 seconds
  to test closed-time expiry; it is not an eight-hour real wait or earned
  King-3 progression. Original earned and source fixtures are untouched.
- `/var/tmp/hvm-clash-donation-rare-return-report` retains that same rare result.
  A second explicitly timer-aged copy sends a real replacement Giant request
  (sequence 3), leaving **0/1** when NPC stock is exhausted: human Giant remains
  1, NPC0, Gold155/XP6. `/var/tmp/hvm-clash-donation-exhausted-report2` is the
  visually reviewed pass. The earlier concurrently run report tapped before
  home finished loading, opened the arena viewer and created no request; it is
  not an exhausted-stock pass. Settled UI input is necessary.
- `/var/tmp/hvm-clash-donation-crash-qualification/summary.json`: private,
  intentionally aborting candidates stop immediately before or after the
  atomic donation checkpoint. Before commit: human/NPC Arrows1/1, request0,
  NPC Gold100/XP0. After commit: 2/0, request1, Gold105/XP1. **Two clean same-file
  relaunches per case** yield 2/0, request1, Gold105/XP1 exactly. These are
  deliberate abort-injection trials, not SIGKILL or power-loss experiments;
  the injections are outside Git and absent from the default guest.
- `/var/tmp/hvm-clash-donation-corruption/summary.json`: **11** actual-client
  corruptions reject with exit70 and every failing file byte-for-byte retained:
  bad header/body, rechecksummed wrong clan/offset/zero clock, duplicate/truncated
  donor, absent donor for fulfilled request, native home/account identity changes,
  and rechecksummed replenishment of consumed stock. Source is immutable.
- Review found a cross-extension migration bug: a checksummed, semantically
  invalid Inbox in a legacy clan was rejected only after donor seeding rewrote
  the file. `/var/tmp/hvm-clash-donation-mail-order/before` reproduces exit70
  with a changed file. Inbox validation now precedes **all** migration/refresh
  writes, including friendly-ID migration. The identical injected file in
  `after` rejects exit70 byte-for-byte unchanged. This is an additional twelfth
  negative case. The final candidate also repeats all eleven donor faults in
  `/var/tmp/hvm-clash-donation-validated-corruption/summary.json`.
- The broad authored guest suite now passes **23** tests, including all-prefix
  truncations, every-byte corruption, rechecksummed malformed GDN1 metadata,
  native donation container/wishlist matching and the four ARMv7 flag variants.
  `/var/tmp/clan-donation-atomic-tests.log`. Shared `src/` is unchanged; reuse
  the qualified runtime/TestLab/compat evidence, not a guessed phone result.

Earlier qualified private default candidate: `/var/tmp/hvm-clash-donation-atomic.ipa`,
SHA-256 `ad76d944ffd86d3c15f3ac48b55a280375cc263729c41a912c15cee2f7f44c53`,
executable `04710bc35d367bc02a54d85e61dc22c4f4ad32b53c67b3621f192dc27f678033`.
Original assets unchanged; no host Clash service/network. Common and rare runs
record nonzero mixed audio with zero dropped blocks/transport errors. This does
not close the owner's intermittent sound or phone performance reports.

Final candidate with the initialization validation-order fix:
`/var/tmp/hvm-clash-donation-validated.ipa`, SHA-256
`4ea1faa388c16cead206763fa1ffd4405f8fd3ee099b9fa0d4816fe2665ecd04`,
executable `9fa1cf936053638b4f6c030ff057e5fcdc0e3fe64c6bc46d231aaccfe8811b81`.
Its `/var/tmp/hvm-clash-donation-validated-report` repeats the actual common
transfer/roster/profile with no crash; its 23-test log is
`/var/tmp/clan-donation-validated-tests.log`. The transaction implementation is
identical to the earlier candidate; the fix moves inbox validation before the
first possible migration write. Assets and shared runtime are unchanged.
`/var/tmp/hvm-clash-donation-validated-return` cleanly reloads the same file:
human/NPC Arrows2/0, request1/10, human Gold2940, NPC105/XP1, no repeat transfer.

Native description/Open→Invite Only changes also retain both actors and the
exhausted request in `/var/tmp/hvm-clash-donation-clan-edit-report`. A same-save
original208 leave clears membership, donor and request without refund or
resource changes; the resulting file has no GDN1 and retains Gold2940.
`/var/tmp/hvm-clash-donation-clan-leave-report`. An actually changed badge still
needs a separate UI trial; the native badge synchronization code alone is not
that qualification.

The final candidate then performs a real paid re-creation on that disbanded
copy: original209 charges **2940→1940 once**, creates clan (1,3), and joins a
new finite NPC to that same clan without charging the human again.
`/var/tmp/hvm-clash-donation-clan-recreate-report`. New-clan NPC starting stock
is an explicitly authored simulation seed after a real 1000-Gold purchase;
ordinary relaunch never reseeds an existing donor.
`/var/tmp/hvm-clash-donation-clan-recreate-return` reloads that same file and
visually retains two members; Gold1940 and the new NPC's100 Gold/clan(1,3)
remain unchanged. No second creation command, debit or NPC is produced.

Committed UI plans: `tools/clash_guest/donor_profile_ui_plan.json` and
`tools/clash_guest/donation_rare_ui_plan.json`. Preserve the cooldown/stock
preconditions and visually review images: “captured” is not “passed”. The
native Local Donor profile exposes Kick/Promote; those operations are still
unported and must not be treated as a full-button qualification. Clan weekly
donation statistics also remain incomplete; profile lifetime count is native.

The older primitive diagnostic and design notes below are retained as the
investigation history. The single synchronous atomic commit above supersedes
the proposed deferred receiver-command journal below.

## Pinned original behavior

The source of truth is the unencrypted 2.1306.1 executable and bundled CSVs,
not modern donation limits or a newer server's packet format. Addresses below
are unslid ARMv7 code addresses; freestanding Thumb calls use address + 1.

| Original routine | Address | Observed contract |
| --- | --- | --- |
| `LogicDonateAllianceUnitCommand` ctor/setData/execute | `0x10398` / `0x103dc` / `0x10438` | Type 203; consumes donor material, awards original Gold/XP, updates native donation accounting |
| Donation encode/decode | `0x103ec` / `0x10410` | Card reference, fixed BE target LogicLong, server-command metadata |
| `LogicAllianceUnitReceivedCommand` ctor/execute | `0x21be8` / `0x21d1c` | Type 211; credits one receiver material using native card creation/update |
| Receive encode/decode | `0x21cc4` / `0x21cec` | Donor String, card reference, server-command metadata |
| `DonateAllianceUnitMessage` encode | `0x10756c` | Card reference, fixed BE request stream ID |
| `DonationContainer` encode/addUnit/canAddUnit | `0xa944c` / `0xa9560` / `0xa9528` | Fixed BE donor ID and count, card refs; actual arena per-donor limit |
| OwnHome message removeHome/removeAvatar | `0x1932c` / `0x19338` | Transfer ownership before packet disposal |
| `LogicGameMode` constructor/destructor/loadHome | `0xd68e4` / `0xd68e8` / `0xd6d00` | Native independent actor with its own default listeners; allocation `0x128`, confirmed by `GameMode::enter` at `0x926e6` |

Arena 1's original request size is **10**, per-donor limit **1**, daily donation
capacity **60**. A common card consumes capacity 1, rewards 5 Gold / 1 XP;
a rare consumes capacity 10, rewards 50 Gold / 5 XP. Those are pinned asset
values, not permission to bypass original stock, rarity, clan, daily or timer
eligibility. Epic eligibility remains unqualified. Donation cooldown uses
native home timers at `+0x98` and `+0xa4` and capacity; guessed timer labels in
an old private inspector were not reliable API contracts.

**Type 203 returns zero even when no card is donated.** Empty stock reaches
its failure listener at `0x1059e`, then the common zero-return exit at
`0x105b4`. In the actual diagnostic, a second command returns zero with stock
0 and unchanged Gold/XP. A controller must inspect native success/failure and
the actual inventory/reward deltas before preparing type 211. Return zero
alone must never cause a receiver grant.

## Reproduce the isolated native component check

Use Linux/WSL with the pinned private IPA, Clang and ld64 described in
[the agent handoff](AGENT_HANDOFF_2026-10-26.md). Nothing is downloaded by this
probe. Keep output, generated arrays, IPAs, saves and game assets outside Git.

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/clash-donation-diagnostic.ipa \
  --build-dir /var/tmp/clash-donation-diagnostic --probe-donation-actors
```

The default build omits the probe. The manifest explicitly records
`donationActorProbe`. Compile checks cover normal, save-profile, probe, and
combined flags; each must retain only the six expected original trampolines
as undefined symbols. Probe markers must be absent from both non-probe objects.
Build timestamps in `initial_login` vary: comparing an earlier default object
with the default candidate here finds only eight differing ASCII timestamp
bytes, not changed instructions. Do not confuse that with identical IPA hashes.

Extract the candidate into a fresh private directory. Run
`tools/clash_guest_ui_probe.py --hvm <qualified hvm> --app <extracted .app>
--home <exact private sandbox copy> --plan <UI plan> --out <report> --gpu
--capture-audio`. App-bundle mode reuses the exact sandbox; IPA-import mode
uses a container root and can replace its installed app. Preserve the original
earned/phone-failing save and retain executable/manifest hashes in the report.

`donation_actor_probe.inc` creates **two independent original starter profiles**,
not references to the live player. It explicitly seeds diagnostic King level 3
to keep a rare donation's XP below the level-up threshold; the donor's ID is
(3,1), its name `Local Donor`, receiver ID (0,1). These are synthetic component
actors, not earned player progress or registered clan members. It selects an
actually owned common, then rare in separate pairs, executes original 203/211,
checks material conservation and original rewards, serializes both original
home/avatar pairs and reconstructs them to verify retained identity/stock/rewards,
checks empty-stock failure after that roundtrip,
and cleans up using original ownership/destructors. Global live native home
size, avatar offset and body checksum must remain unchanged. No receive command
is issued to the live player or after the failed second donation.

Initial actual-client diagnostic evidence, before adding the native roundtrip
check: `/var/tmp/hvm-clash-donation-actor-diagnostic-report`, host
`/var/tmp/hvm-clan-core-work/build/hvm` (912a87e runtime; shared source tree
identical to 5666195). Package SHA-256
`ad05d31b3a3a27541216e2d255d298086dc9511db1659188e1599d0e7030742d`,
executable `0e42309f7c1b387efde20275e1a8799390a4f915d8d4f7ad7770e4187b9ae656`.
Common: stock 1→0, receiver 1→2, Gold 100→105, XP 0→1.
Rare: stock 1→0, receiver 1→2, Gold 100→150, XP 0→5.
Both empty-stock retries preserve amounts. Exit 0, no app crash; normal home
and clan frames captured. Mixer records 555008 music frames, 176128 effect
frames and zero dropped blocks/transport errors. This is signal/path evidence,
not a listening-quality, phone-stutter or full-button qualification.

The roundtrip candidate also passes both cases, rejects a second donation from
the restored exhausted donor, and leaves the live checkpoint body unchanged:
`/var/tmp/hvm-clash-donation-actor-roundtrip-report`. Package SHA-256
`bf9427e9082c484ece2912270c9fe246ed5c74f2f16da0ad7ca77eee8a79e7dc`,
executable `8481cd3cde126dcd61c3a9280f017bf3cbd648043747f5928f3b7c5f8e3350df`.
Exit 0, 344064 music / 176128 effect mixer frames, zero dropped audio blocks.
The first plan's second tap accidentally opened Cards: visual review identified
that, and it is not counted as a clan-page pass. The corrected same-save plan
and its separate `/var/tmp/hvm-clash-donation-actor-roundtrip-return-report`
pass exit 0 with visual review of the actual clan: wallet 2940 / gems 87,
persisted Arrows request 0/10, original chat and terminal friendly entry.
There is still one actual member and no live donations. The 22-test
suite passes in `/var/tmp/clan-donation-actor-roundtrip-tests.log`.

## Remaining implementation order

1. Add an explicitly local NPC member with persisted original home/avatar,
   bounded finite stock and an honest roster/profile. Do not reset inventory on
   login, fake online humans, or fabricate donor names on an empty request.
2. Validate actual membership, card/rarity/arena eligibility, request age and
   capacity, per-donor limit, original cooldown/daily usage and owned material.
   GCR1 currently requires zero donors; merely relaxing that parser is unsafe.
3. Bind donor post-state/reservation and immutable receiver command to the same
   atomic CRG1 checkpoint and request ID. Preserve the receiver pre-state until
   its original command is applied. Validate actual native deltas and distinguish
   pending/applied states on restart; never replay a completed grant. Qualify
   forced-stop recovery at each boundary and reject corrupt files unchanged.
4. Update the native DonationContainer/stream and live UI, exercise Donate,
   profile/info, replacement/expiry, leave/re-create and repeated relaunches.
   The current profile route only accepts the human (0,1); it needs real NPC
   data before a roster entry becomes clickable. Namespace IDs separately from
   chat (0), requests (1), and friendly history (2).
5. Qualify supply replenishment through actual native timed rewards/progression;
   no per-login refills or unbounded free receiver materials. Test common and
   rare, exhausted stock, duplicate delivery, request replacement and later
   card upgrades together. Do not enable the native progress simulator as a
   substitute for a donation transaction.

Then continue the owner's ordered authored post-training names, arena-legal
varied decks/actual native AI behavior, original daily shop and remaining
arena/result/UI/audio/performance work. Preserve original training names.
