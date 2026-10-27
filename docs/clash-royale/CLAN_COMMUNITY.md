# Local invitations, retained rejoining and weekly donations

Continuation after `d213fdf` on `codex/clash-offline-continuation`, October27.
The pinned ARMv7 2.1306.1 client now exposes the actual persisted NPC profile
after removal, accepts its original Invite button and displays weekly donation
scores in the original clan header/roster. The game/transport/save remain inside
the guest IPA; shared HydrogenVM source and pinned assets are unchanged.

## Native interaction

One clearly labelled offline-clanmate ChatStreamEntry is persisted per clan,
namespace4/id1, author/account(3,1), with original name/level/role snapshot.
It is separate from human chat0, human requests1, friendly2 and NPC requests3.
Its text is authored functionality, not invented historical join activity or
a real online player. The original context menu opens View Profile; a retired
NPC really has No Clan. Its original Invite button appears only when the player
has a clan and the pinned role's CanInvite byte at `+0x4c` permits it. The role
CSV allows Leader/Elder/Co-leader, not ordinary members. Current two-actor
leader invariants still apply; no permissions or UI assertions are disabled.

Original SendAllianceInvitationMessage14322 (`0x110210`) carries a fixed BE
target ID. The handler accepts only the persisted local NPC and actual eligible
home/player. That NPC auto-accepts through original JoinAllianceCommand209,
create=false (`0xe5874`), on its retained native actor. It uses the current clan
name/badge and joins as Member, without charging the player, replacing the NPC,
refilling stock or clearing its timers. Only after the joint atomic checkpoint
does original empty24322 (`0x380ec`) acknowledge the invitation. Current clan
data/conversation refresh then shows two members; a repeated already-committed
acknowledgement cannot repeat native209 or its rewards/progress.

The native profile popup retains its previously loaded No Clan text until it is
closed; the original invitation toast appears, and reopening the roster/profile
shows current membership. The offline message keeps the NPC reachable through
future kicks without a fake Facebook/Game Center directory or automatic rejoin.
Leaving/disbanding the whole local clan clears this clan's community metadata;
ordinary kick/rejoin preserves it and the finite inventory provenance.

Original209 legitimately increments the NPC's join-achievement progress
(`updateAchievementProgress` at `0xe5976`, observed resource60000000 +1).
Qualification allows exactly this native delta, preserving other economic
fields. An early private test expected equality of every counter and failed
that expectation; the client itself exited0. No native achievement was reset
and no game assertion was suppressed to obtain a passing result.

Original208 starts the pinned leave donation cooldown (global150 minutes).
209 does not bypass it. A new actual Knight request after rejoining a stocked
NPC remains0/10 during the tested cooldown. Current official
[donation guidance](https://support.clashroyale.com/hc/en-us/articles/49484955716891-Requesting-Donating-Cards)
explains the general need to prevent clan-hopping for Gold; it is not evidence
for this archived client's exact timer or the old server's weekly reset clock.

## Weekly policy and display

The pinned client names `Donations/Week` and encodes native member/header
donations at `+0x2c` (`getDonations`0x37f10/0x38454). Its shipped data does not
specify the retired server's weekly reset schedule. The **offline policy** is
Monday00:00 UTC, using native donation-capacity points: common1, rare10. The
historical server's exact reset hour/weekly weighting is not claimed verified.
This does not add later-era Epic Sunday or Clan War systems.

Only newly completed original203/211 transfers count. Sender/recipient points
change together with both native actors, inventory ledger and receipt proof in
the existing fsync/rename. Roster rows show each current member's sent points;
the header sums current members. Received points are retained for cross-actor
validation. Retired NPC scores remain stored but are excluded from the header
until rejoining. Lifetime native donation totals still count actual cards and
are never reset by the weekly calendar. Wallets, XP, cards and cooldowns are not
changed by a weekly reset.

Legacy event dates are unavailable: migration starts weekly counts at zero,
without assigning lifetime totals to guessed weeks. The next forward calendar
boundary resets scores; a clock moving backward does not erase them again.
The check runs once per minute at home and when opening clan data. Additional
NPC supply currently comes from the actual player-donation path; no login refill
or invented earned battle/chest stock was added.

## GCM1 and save compatibility

GCM1 contains fifteen little-endian u32 words /60 bytes: tag `0x314d4347`,
version1, clan serial, week-start epoch, human sent, NPC sent, human received,
NPC received, profile-message epoch, entry size, human-body FNV, NPC-body FNV,
entry FNV, profile sequence1, header FNV over56 bytes. The canonical native
ChatStreamEntry follows, bounded256 bytes. Stored age is0; rendering derives
age without rewriting history. Profile identity/text are exact bounded values.
Native-body bindings and reciprocal weekly totals validate before any migration.

GCL1 required-community bit `0x800` requires donor/inventory bits `0x300` and
status2..4. It does not require a request that the NPC has never made. Missing,
duplicate, malformed or cross-bound community state fails with the original
file retained. Fully enabled flags are0xf00 plus status. Older guests reject
the new parent bit: do not downgrade saves. Existing [GNI1](DONOR_INVENTORY.md)
and [GNR1](RECIPROCAL_DONATIONS.md) formats remain readable and unchanged.

## Reproduction and evidence

```sh
python3 -m unittest discover -s tools/tests -p 'test_*clash_guest*.py' -v
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-community-v1.ipa --build-dir /var/tmp/hvm-clash-community-v1-build
python3 -m zipfile -e /var/tmp/hvm-clash-community-v1.ipa /var/tmp/hvm-clash-community-v1-run
python3 tools/clash_guest_ui_probe.py --hvm /var/tmp/hvm-clan-core-work/build/hvm \
  --app '/var/tmp/hvm-clash-community-v1-run/Payload/Clash Royale.app' \
  --home PRIVATE_COPIED_SANDBOX --gpu --capture-audio --startup-seconds 12 \
  --plan tools/clash_guest/clan_invite_ui_plan.json --out NEW_REPORT_DIRECTORY
```

Use the plan's exact copied-fixture preconditions and review screenshots plus
native save deltas. Captured steps alone are not a pass. Private helper task
`outputs/community_qa.py` uses read-only inspector dependencies and isolated
`/var/tmp/hvm-clash-community-qa` sandboxes. Never run inspection-only normalized
files or replace the preserved earned/phone-failing instances.

IPA SHA256 `362ad2026b2968f12e272a0a54f87c78d81b1c5b31bb824b54d9213087ed1c8e`;
executable `484444796bd0fad7b76a5ae2f39fca44572f4224eacbdf94661087eb28a8b3e7`.
Unchanged engine `/var/tmp/hvm-clan-core-work/build/hvm`, qualified912a87e,
shared src tree `9b29155c175d76eda08111072700403272a19d7e`. Earlier runtime
fulltest260/260, TestLab18/18 and compatibility evidence remain the engine gate.
No fresh iPhone, performance, audio-quality or power-loss claim is made.

- 29 guest tests pass: four ARMv7 variants keep expected imports; independent
  UTC calendar vectors include week boundaries, leap-year and unsigned32-bit
  range; GCM1 bindings/cross-totals/size guards, fixed NPC wire identity/text
  and required-parent combinations are covered without proprietary fixtures.
- Actual native View Profile → Invite → success toast → roster2/50, then two
  normal returns, retains NPC inventory/wallet/XP and exactly one native join
  achievement increment. A real stocked-NPC request stays0/10 after rejoining,
  exercising the preserved native leave donation cooldown.
- Actual common Archers Donate produces weekly1; later actual rare Fireball
  Donate produces11. An actual player Knight request receives one NPC material,
  giving roster11/1 and clan12. Native original rewards, stocks and complete
  state persist through two returns. Later NPC request selection uses explicitly
  eight-hour-aged **copies**, not real elapsed waits or automatic supply.
- A prior-week metadata fixture exercises the weekly zeroing path and same-save
  return, retaining both complete native economies/inventory/lifetime totals.
  Missing-required GCM1, wrong NPC-body binding and mismatched cross-actor totals
  each reject exit70 with byte-identical files on separate damaged copies;
  the frozen source remains unchanged. The final `summary.json` records these
  passes. This fixture is not a real seven-day wait.

Reports/images are under `/var/tmp/hvm-clash-community-qa/`; top-level logs
`/var/tmp/clash-community-tests2.log`, `clash-community-invite.log`,
`clash-community-stats-final.log`, `clash-community-rejoin-cooldown.log` and
`clash-community-rollover.log` record exact outcomes. Initial exploratory rare
coordinates hit the moved chat message; this did not count as a donation pass.
The corrected isolated final flow and inspected roster prove11/1/12. The earlier
invite harness's overly strict achievement expectation is described above.

Next: authored randomized post-training opponent handles (retain trainers),
arena-legal varied decks and native AI styles, original daily shop; later arenas,
draw/results/profile counters and full button/device/visual/audio/stutter audit.
Autonomous NPC earned stock beyond player donations remains a separate future
extension. The shared emulator is alpha; required engine changes must still
pass independent runtime/TestLab gates and the affected real app.
