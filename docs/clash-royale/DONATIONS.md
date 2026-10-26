# Native donation work — October 26, 2026

Status: native donor/receiver primitives are qualified in an explicitly opt-in
diagnostic. **Production clan requests remain unfulfilled.** There is no durable
NPC member, donation inventory, recovery journal or live Donate button yet.
Do not package the diagnostic as a completed clans feature.

This follows Kimi's `96003cb` friendly lifecycle and `5666195`'s independently
reproduced post-friendly upgrade/relaunch and stream-ID fixes. The guest owns
its offline transport and save; no title backend runs in HydrogenVM. Shared
emulator `src/` is unchanged in this donation investigation.

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
