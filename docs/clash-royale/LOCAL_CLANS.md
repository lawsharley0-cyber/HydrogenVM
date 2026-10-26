# Local clans and subsequent simulation work

Owner order, October 26: finish the current timer/recovery/control work, then
make clan creation/interactions persistent offline; randomize player-style
opponent names only after training; vary legal arena decks and actual bot
playstyles; restore the original daily store. Training names stay original.
The timer checkpoint is 0c24408/v33. v42 now qualifies persistent solo-clan
creation, chat, info/settings/search and leave/re-create in synthetic component
fixtures. Clan card requests/donations, mail and friendly battles are not ported;
this is not the completed clans feature or a full-button pass.

## Current component qualification — v42

- Exact host source cbde492, Linux/WSL GPU path. Shared input passes 260/260
  fulltest and 18/18 TestLab cases, with TL-R failing before the fix. The full
  report and zero-regression compatibility report are indexed below. Twelve
  authored guest tests now include independent reference classes, every-prefix
  truncation/single-byte storage corruption, invalid sizes/wallet states, chat
  author/age/boundaries, and freestanding ARMv7 normal/profiling compilation.
- Actual original command **209** charges 1000 Gold, sets identity/name/badge,
  leader role 2 and the native clan achievement. Journal contains the pre-charge
  avatar and immutable command, not a copied post-charge avatar plus another
  live debit. `GameMode::addCommand` 0x93560..0x93576 assigns executor (0,1)
  before the native echo; this differs from the transmitted default (0,0).
- The first actual creation at v36 persisted 5000→4000 before an overly strict
  echo check aborted. v37/v39 then recover that already-completed save without
  another charge. The distinct v41 **owned SIGKILL** trial catches a valid
  pending GCL1 before delivery; relaunch of the same file executes 209 and
  commits 4000 once. `/var/tmp/hvm-clash-clans-{interruption,recovery}-v41`.
- Original native clan page/info renders the actual local member, leader role,
  score and badge. `AllianceMemberEntry::getName` 0x37f00 reads +0x1c; +0x18
  is a different nullable identity string. Native region data reference is
  **class 57**, not class 1; header region is +0x34. Earlier component candidates
  exposed and corrected these schema mistakes; they are not releasable builds.
- Original chat request 14315 produces native ChatStreamEntry type 2 through
  its original encoder, commits it before 24312 delivery, and restores via
  24311. Keep at most 64 entries/32 KiB, with real wall timestamps and computed
  ages; no fake remote authors. Original online-count 20207 describes the one
  actual running local member. `/var/tmp/hvm-clash-clans-{chat-v39,recovered-chat-v41,chat-return-v42}`.
- Settings 14316 stores the actual description/type/badge/requirements/region
  atomically and delivers original **212** for native badge/UI updates. Actual
  description and Open→Invite Only survive relaunch. Search 14324 honors native
  name/region/member/score/joinability filters over only this stored clan;
  native String lowercasing/contains produce the actual matching 24310 result.
  `/var/tmp/hvm-clash-clans-{edit-v39,search-v41}`.
- Leave 14308 journals original **208**. Native membership clears, no Gold is
  refunded, chat clears when the only member disbands the clan. The serial
  survives; the next actual creation uses (1,2), charges 4000→3000, and retains
  the original creation achievement progress. `/var/tmp/hvm-clash-clans-{leave-v41,recreate-v42}`.
- Eight private corrupted-checkpoint trials reject bad headers/payloads,
  duplicate/truncated extensions, a wrong immutable command with recomputed
  checksums, and an out-of-bounds chat record with recomputed checksums. Each
  aborts at its explicit guard with the damaged file byte-for-byte unchanged;
  the source fixture remains untouched. `/var/tmp/hvm-clash-clan-corruption-v42`.

These are labeled King-3/Gold-5000 fixtures, not earned King-3 progression or
iPhone qualifications. No phone save or original earned source is reset.
Private v42 package: `/var/tmp/hvm-clash-guest-v42.ipa`; no package/assets/save
is committed or delivered as complete. Pre-purchase controls need separate
settled steps: a captured frame is not proof a too-early tap reached Create.

Storage: GCL1's ten LE u32 words are tag, monotonic clan serial, operation
status (0 disbanded / 1 create pending / 2 active / 3 leave pending / 4 settings
pending), request length, immutable command length, before/after creation Gold,
native-body FNV, XOR of the two payload FNVs, and header FNV over 36 bytes.
The bounded original create-request bytes and command follow. GCS1 has seven
LE words: tag, payload length, entry count, sequence, native-body FNV, payload
FNV, header FNV over 24 bytes; each payload record has BE epoch/length and the
native encoded entry. Validate the complete state before refresh/clock writes.
All extensions share CRG1's existing atomic fsync/rename transaction. This is
corruption detection, not cryptographic authentication. Older guests reject
these extensions: do not downgrade an instance after creating a clan.

Apple status: production simulator shell and Linux CI compile/pass; the
isolated UI harness first lacked newer bridge members (fixed 9a49f8c), then
XCTest classified the new UIKeyInput viewport as TextView instead of Other.
fa5a955 queries the same stable identifier across element types, retaining all
coordinate/input assertions. That run then hits the 15-minute combined-step
timeout after a 6:35 cold simulator boot; it is not an input result. 379a8a5
separates harness build, checked simulator boot and test-without-building, with
independent bounded timeouts/logs. Its UI run is pending at this snapshot.
No claim of real iPhone keyboard/gameplay qualification follows from compilation.

Reproducible authored plans are `tools/clash_guest/clan_{create,chat,info,edit,search,leave}_ui_plan.json`.
Run create only against an explicitly labeled King-3 component instance with
sufficient Gold and no clan; it spends the real original 1000 Gold. Subsequent
plans need actual completed membership; leave disbands the sole-member clan.
Use the ordinary `clash_guest_ui_probe.py --gpu` path, review every captured
frame and inspect the persisted result. A clean exit/captured step is not a
visual pass. Keep the actual earned King-2 player separate.

Final clean-source 912a87e gate: full report build 1/CPU 15/iOS 6/bench 5 all
pass, TestLab 18/18 and focused heap/entry/state/service suites pass. Full
compatibility has zero regressions against baseline and previous report, with
three informational timing deltas and the optional original Clash skip.
Indexed report: `docs/dev-system/reports/linux-x86_64/20261026T150956Z-912a87e64a33.json`.
No emulator C++ change is added by this clan-core checkpoint. Current match
entry also refuses any still-pending clan transaction before another debit.

## Authoritative client evidence

Pinned executable/IPA hashes and private build instructions are in
[GUEST_MOD_PROGRESS](GUEST_MOD_PROGRESS.md). Facts from its globals.csv:
ALLIANCE_UNLOCK_EXP_LEVEL=3, ALLIANCE_CREATE_COST=1000,
ALLIANCE_CREATE_RESOURCE=Gold. Keep those gates in the production guest.

An isolated, explicitly synthetic King-3/Gold-5000 component fixture is at
`/var/tmp/hvm-clash-clans-k3-fixture`; it derives from the earned save but
changes only the labeled test level/wallet, with corrected checksums. The
earned player remains King 2 and is never restored/reset from that fixture.
Its first native level-up/legal screens render. Actual creation and subsequent
membership/chat/edit/leave/relaunch behavior still need implementation/tests.
After those screens close, the actual Clans tap aborts v33 on 14303 in
`/var/tmp/hvm-clash-clans-open-before-v33`; the empty discovery reply is the
first concrete guest protocol gap.

Native schema/function anchors (Thumb addresses below omit the Thumb bit):

| Function/message | Evidence |
|---|---|
| CreateAllianceMessage 14301 | encode 0x2f718: BE name/description strings, badge reference, type VInt, required-score VInt, region reference |
| AskForJoinableAlliancesList 14303 | type getter 0x75cac; empty request |
| JoinableAllianceList **24304** | getter 0xa1544; encoder 0xa13a8 writes VInt count plus native AllianceHeaderEntry objects; do not assume request+10000 |
| AllianceList search response | encoder 0x65a44: search BE string, VInt list count, headers |
| AllianceDataMessage | 0xb4ae0..0xb4c1c; body is native AllianceFullEntry |
| AllianceFullEntry | 0x690a8: header, description string, member-count VInt and native members |
| AllianceHeaderEntry | constructor 0x38200/encoder 0x38220; original badge/region references and integer fields |
| AllianceMemberEntry | constructor 0x37d58/encoder 0x37d84; original name, identity, arena, role/level/score/donation fields and optional home ID |
| Native join server command **209** | 0xe5874 execute, 0xe59a4 encode, 0xe59e0 decode: fixed LogicLong, name, badge reference, create boolean, server-command base metadata |
| Join execution | original resource/cost getters 0x68b50/0x68b54; clamps to wallet, charges original helper with reason 4, sets original membership fields, creator role 2, achievement update and UI callbacks |
| Membership setters | 0x9ae04 ID, 0x9ae28 name, 0x9ae34 badge, 0x9ae3c role |
| Clan stream/chat | AllianceStreamMessage 0x7c324; ChatToAllianceStreamMessage 0x11ffcc; schemas pending complete qualification |
| Leave/settings | LogicLeaveAllianceCommand 0x1254c; ChangeAllianceSettingsMessage 0x11ec80; original schemas still under investigation |

Local state must describe this player's actual local clan, not invented remote
players or successful network calls. Keep membership, original creation cost
and clan metadata in one durable transaction; interrupted UI delivery must not
lose the clan or charge twice. Native join/leave/settings executors and UI
callbacks must retain their original behavior. Qualify each interaction and
reject unsupported requests explicitly until its actual implementation exists.

## Reference comparison

The [royale-proxy protocol repository](https://github.com/royale-proxy/cr-messages)
is a primary implementation reference. Its
[AllianceData](https://github.com/royale-proxy/cr-messages/blob/master/server/AllianceData.json),
[header](https://github.com/royale-proxy/cr-messages/blob/master/component/AllianceHeaderEntry.json)
and [member](https://github.com/royale-proxy/cr-messages/blob/master/component/AllianceMemberEntry.json)
definitions are dated 2017 and have extra fields absent from this pinned
native encoder. Reusing those packet bytes would be incorrect. They help
identify concepts; the pinned original encoder/decoder is authoritative.

[Supercell's current clan guide](https://ingame.help.supercellsupport.com/clash-royale/en/articles/creating-or-joining-a-clan.html)
describes clan creation/customization generally. It is not evidence for this
old client's exact level gate, field order, limits or economy. Those come from
the pinned assets/code and actual UI tests.

## Shared prerequisite investigation (before bf6dd7f)

The current emulator marks UITextField/UITextView as editing but has no host
keyboard bridge or queued CLI text input. It also lacks UIKeyInput insertion
and deletion. Initial-name confirmation with its default value did not qualify
typing. Clan names/search/chat require shared text-input work, with independent
TestLab coverage and real client interaction, not hardcoded clan names.
Shared Groups D/E are reserved for this necessary keyboard/input prerequisite
on the same owner-authorized continuation. Pre-change fulltest/TestLab and a
new minimal failing TL-R must precede runtime fixes.

Pre-keyboard exact runtime 5fbaa8d repeats fulltest 256/256 and TestLab 17/17
in `/var/tmp/hvm-keyboard-pre-{fulltest.log,testlab}`. New authored TL-R stops
that unchanged binary with code 70 on missing `-[UITextField insertText:]`;
`/var/tmp/hvm-keyboard-tlr-before/testlab_report.json` preserves the diagnostic.
The new case specifies literal UTF-16 ranges, delegate refusal, edit events and
focus refusal independently of emulator output. UIKeyInput edit support is
being implemented before the external CLI/native keyboard route; neither this
partial step nor its compile establishes working clan creation.

bf6dd7f's clean build at `/var/tmp/hvm-keyboard-work` passes fulltest 256/256
and all 18 TestLab cases, including TL-R. The v34 internal guest adds the
qualified 14303→24304 empty directory; original Clans/Create forms now render
in the synthetic fixture without the former discovery abort. Actual creation
is still unported. Queued CLI text/Backspace/Return, native host UIKeyInput
routing and stale-focus tests are the next candidate, with Apple validation
required. Desktop shortcuts move to Ctrl+S/B/F so ordinary name characters
do not trigger screenshot/background actions.

The Apple UI harness also needs its actual BinaryEditor source and a defined
crash-journal interface. Its synthetic bridge now uses an actual closable
FileHandle rather than a successful no-op; that does not test CPU signal
handlers. A native keyboard-routing UI test is added to the harness. Neither
fixture nor green UI tests qualify real iPhone guest gameplay.

Input contracts: [UIKeyInput](https://developer.apple.com/documentation/uikit/uikeyinput),
[replacement delegate](https://developer.apple.com/documentation/uikit/uitextfielddelegate/textfield(_:shouldchangecharactersin:replacementstring:))
and [editingChanged](https://developer.apple.com/documentation/uikit/uicontrol/event/editingchanged).
The UIKeyInput bridge supports simple entry; full marked-text/IME behavior must
not be claimed from insertion/deletion tests.

Main has advanced to 1172b74 after the integrated 5968d88. It is fetched for
review, not merged into this continuation yet. In particular its new
finishCrash termination branch converts faults during shutdown into a clean
close; review that reporting policy against the no-hidden-assertion constraint
before integrating. Existing immutable test runtime remains 5fbaa8d.

The UI probe can now reuse an installed .app with --app and an explicit exact
guest sandbox, recording executable/Info.plist hashes instead of an IPA hash.
This avoids re-import and preserves test provenance if a downloaded package is
removed. It never recreates deleted downloads or resets saves.

Later research queue: inspect the original NPC decision code and compare
several primary bot/simulator projects before selecting style/deck variation;
verify actual behavior, not just bot labels. Current modern shop documentation
includes items absent from this 42-card build and must not define its rotation.

cbde492 completes the shared keyboard route. Exact clean Linux build passes
fulltest 260/260, TestLab 18/18 (TL-R fails on unchanged 5fbaa8d), focused
heap/entry/native state/service suites, nine guest tests and the full report
(build 1, CPU 15, iOS 6, bench 5; no skips/failures). Full compatibility has
zero baseline/previous regressions; its optional original Clash fixture skips,
so modified-client interaction is qualified separately. Indexed report:
`docs/dev-system/reports/linux-x86_64/20261026T082000Z-cbde4922f25a.json`.
The actual original clan-name field accepts UTF-8 text through queued host
input and enables its Create button in `/var/tmp/hvm-clash-clans-name-input-v34`.
This is a synthetic King-3 component fixture, not earned clan progression.
Creation has not been purchased/ported. Apple compilation/keyboard UI tests and
real iPhone input remain pending; Linux output is not that evidence.
