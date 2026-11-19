# Playable local NPC requests and player donations

The subsequent [clan community stage](CLAN_COMMUNITY.md) adds native invitation/
retained rejoin, reachable NPC profile history and weekly donation points.
The following sections preserve this reciprocal-transfer stage's qualification.

Continuation after `a51c68e` on `codex/clash-offline-continuation`. Local Donor
now requests an owned starter card through original524. The player can use the
original green Donate button, receive native Gold/XP and see the native request
bar update. Both actors, native cooldown, inventory provenance and receipt proof
persist together. This completes the scoped reciprocal donation interaction,
not the whole Clans feature or full offline game.

## Behavior and original rules

- The explicitly local NPC requests Knight, Archers, Giant, Bomber, Fireball and
  Arrows in sequence, trying only cards accepted by original524. No tutorial,
  membership, maximum-card or cooldown check is bypassed. Original request
  cooldown is eight hours; elapsed time uses the persisted NPC clock. A bounded
  check runs once per minute at home, with no per-frame actor decode or refill.
- NPC request namespace3 is distinct from human request1, friendly2 and chat0.
  The latest NPC request merges chronologically with existing conversation.
  Deltas wait until the first full snapshot: sending a new NPC entry before
  that snapshot made the native cache display it above old history. Final v2
  fixes this and shows the newest request below prior entries on first login.
- Original14310 contains a card reference then fixed BE stream ID. The handler
  validates the exact request/card and calls original
  `DonateStreamEntry::canAddDonation` at `0x1108fc` on the decoded entry, actual
  player identity/card and arena. Native capacity/per-player rules decide
  eligibility. Original203 also enforces stock and daily donation rules.
- Original203 runs on the live player; original211 runs on the detached NPC.
  The player spends one actual material and receives the native rewards, with
  no custom Gold/XP grants or frozen level/XP assumptions. NPC stock rises one;
  GNI1 received history and the native donor list change in the same atomic
  checkpoint. A zero result alone is not success: stock and native donation
  counter must change. Only after commit does24312 update the conversation.
- In the tested Arena-1 player fixture, one common card fills1/10 and further
  clicks are blocked despite the player retaining another card. A rare fills
  1/1 because its native capacity is10. The common bar is not fabricated into
  10/10 by inventing nine additional donors. Native rewards are5 Gold/1 XP
  common and50 Gold/5 XP rare; lifetime donations count cards, not capacity.
- Replaced/expired/retired request targets receive native24318 removal, never a
  fake successful transfer. Kick clears the active request but retains its
  sequence, NPC inventory and provenance. Disband clears that clan's NPC state.
  No relaunch rejoin/refill, remote human directory or automatic stock supply.

## Durable GNR1 extension

Nine little-endian u32 words /36 bytes: tag `0x31524e47`, version1, entry size,
sequence, creation epoch, human native-body FNV, NPC native-body FNV, entry FNV,
header FNV over32 bytes. Canonical native entry follows, bounded2048 bytes,
stored age0; rendering computes age without rewriting history. Sequence1..1e9
survives removal; size0 requires epoch0. Namespace/author/donor IDs and card-list
bounds validate independently. NPC receipt proof cannot exceed its cumulative
GNI1 received history. Both native-body bindings validate before any migration.

GCL1 adds required-request bit `0x400`; it requires both `0x100` donor and
`0x200` inventory bits and status2..4. Removing the whole required GNR1 rejects
with the save untouched. The single existing fsync/rename commits all records;
there is no second supply file. Old guests reject the new parent flag: do not
downgrade an updated save. [DONOR_INVENTORY.md](DONOR_INVENTORY.md) describes GNI1.

## Verification

Unchanged runtime: `/var/tmp/hvm-clan-core-work/build/hvm`, qualified912a87e,
shared source tree `9b29155c175d76eda08111072700403272a19d7e`. No `src/` changes.
Prior fulltest260/260, TestLab18/18 and compatibility evidence remain applicable
to that engine. This stage does not claim new iPhone, sound-quality or frame-time
qualification. All game-component tests use copied synthetic King3 clan states;
the original earned and phone-failing saves remain untouched.

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-reciprocal-v2.ipa --build-dir /var/tmp/hvm-clash-reciprocal-v2-build
python3 -m zipfile -e /var/tmp/hvm-clash-reciprocal-v2.ipa /var/tmp/hvm-clash-reciprocal-v2-run
python3 tools/clash_guest_ui_probe.py --hvm /var/tmp/hvm-clan-core-work/build/hvm \
  --app '/var/tmp/hvm-clash-reciprocal-v2-run/Payload/Clash Royale.app' \
  --home PRIVATE_COPIED_SANDBOX --gpu --capture-audio --startup-seconds 12 \
  --plan tools/clash_guest/npc_donation_ui_plan.json --out NEW_REPORT_DIRECTORY
```

Review screenshots and decoded save deltas; `captured` is not a pass. The plan
states its exact history/stock preconditions and coordinates. Never import a
replacement IPA over the preserved source sandbox.

Final v2 IPA SHA256 `aa0ceac8fb19184d9b9e0af77d31914b49f7f8c50a34f2207c38eade91b5bdb5`;
executable `33deb596a99c0572c448c11fc6f13c4dd12e2a5618df36d73bf6f77587cba8c3`.
Private v1 differs only in the pre-snapshot delta guard; transfer rules are the
same. No proprietary IPA/assets, generated fixtures or player saves enter Git.

- 27 guest tests pass, including four ARMv7 compile variants with no extra
  runtime imports, explicit NPC/human namespace and donor separation, bounded
  multi-card containers, GNR1 storage/retirement and required-parent combinations.
- Actual v1 native UI: Knight human2→1/NPC1→2, Gold2940→2945, XP6→7,
  request1/10 and GNI received1. Repeated taps grant nothing; two returns retain
  complete economy and history. An initial coordinate probe hit the human's
  own request and showed the correct self-donation rejection; it is not counted
  as a donation pass. The later repeat plan's second-donation expectation was
  incorrect: the native limit blocks it; stored stock/reward proof takes precedence.
- Explicitly NPC-clock-aged **copies**, not real eight-hour waits, advance
  requests2/3/4/5 through Archers/Giant/Bomber/Fireball without inventory or reward
  changes. Actual Fireball Donate gives human1→0/NPC1→2, Gold2945→2995,
  XP7→12 and native1/1. Two returns retain it.
- Another explicitly aged copy creates Arrows request6. Actual player Donate
  replenishes previously spent NPC Arrows0→1, human2→1, Gold2995→3000, XP12→13,
  lifetime donations3. Ledger sent1/received1 and both the old fulfilled human
  request and new NPC receipt survive two returns. This is real UI replenishment,
  superseding the prior test-only native211 supply fixture.
- Final v2: fresh first-login request shows in the correct order, actual Donate
  and return retain one transfer; missing-required and recomputed wrong-NPC-body
  binding each reject exit70 with byte-identical damaged files. Actual original
  Kick retires the request and preserves supplied stock/history through return.

Private evidence/helper: task `outputs/reciprocal_ui.py` and read-only
`inventory_qa.py`; reports `/var/tmp/hvm-clash-reciprocal-ui-*`, summary
`/var/tmp/hvm-clash-reciprocal-ui-summary.json`, final negative copies
`/var/tmp/hvm-clash-reciprocal-final-negatives`, final kick sandbox
`/var/tmp/hvm-clash-reciprocal-final-kick`; logs
`/var/tmp/clash-reciprocal-final-tests.log`, `clash-reciprocal-supplied.log` and
`clash-reciprocal-final-ui.log`. Use the exact sandbox and extracted app with GPU;
no host backend flags. This stage adds no new deliberate transfer-abort matrix
or power-loss claim; the existing atomic writer/recovery qualification remains
in [DONOR_INVENTORY.md](DONOR_INVENTORY.md).

## Remaining owner queue

Finish explicit NPC invitation/rejoin with retained stock, additional legitimate
NPC supply and weekly donation statistics. Then authored randomized post-training
opponent names (keep trainers), arena-legal varied decks/native AI styles and
the original daily shop; later arenas/results and the full button/device,
visual/audio/stutter audit remain open. Shared alpha emulator changes may still
be required and must pass its independent runtime/TestLab gates.
