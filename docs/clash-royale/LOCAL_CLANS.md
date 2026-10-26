# Local clans and subsequent simulation work

Owner order, October 26: finish the current timer/recovery/control work, then
make clan creation/interactions persistent offline; randomize player-style
opponent names only after training; vary legal arena decks and actual bot
playstyles; restore the original daily store. Training names stay original.
The timer checkpoint is 0c24408/v33; this document tracks the next stage and
does not claim clans are implemented yet.

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

## Shared prerequisites and open work

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
