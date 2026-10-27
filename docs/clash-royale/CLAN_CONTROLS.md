# Native local clan roles and removal

Continuation after `127ad31`. The target remains the pinned ARMv7 2.1306.1
client, with its own guest-local transport and atomic save inside HydrogenVM.
This component adds Promote, Demote, leadership handoff and Kick for the actual
persisted Local Donor. It does not complete reciprocal donations, invitations,
NPC supply, weekly statistics or the full Clans/button/device audit.

## Original rules and protocol evidence

The pinned `res/csv_logic/alliance_roles.csv` defines NonMember0, Member1,
Leader2, Elder3 and Co-leader4. CSV `Level` is rank strength, not the role ID.
The original UI shows Member→Elder→Co-leader→Leader confirmations; Elder and
Co-leader show Demote where Member shows Kick. The old native rank comparator
at `0x38024` and promotion permission helper at `0xa9794` decide hierarchy;
original role data's CanKick byte at `+0x50` is also checked. The controller
does not grant authority based on a button label or return-code-only no-op.
Leadership goes to an eligible Co-leader, with the former human leader becoming
Co-leader. Exactly one leader must remain in the stored two-actor model.

[Supercell's current role guide](https://support.clashroyale.com/hc/en-us/articles/49484954095771-Clan-Roles)
is primary context for rank permissions and a leader handing leadership to a
Co-leader. It contains newer Boat/War features and is **not** evidence for this
2016 client's wire format, exact role IDs or cooldown fields. Those come from
the pinned original code/assets and actual native dialogs/interaction.

| Operation | Original pinned behavior |
| --- | --- |
| Change role | 14306: fixed BE target ID, VInt new role; 24306 echoes that ID/role after commit |
| Role mutation | Native server command210, constructor `0x417bc`, execute `0x41800`: sets the actual avatar's role in the matching clan |
| Kick intent | **526 = 0x20e**, type getter `0x76c4c`; encode `0x76c54` writes base tick/sequence/executor first, then String and fixed BE target ID |
| Kick transport | 526 arrives inside 14102 EndClientTurn. It is an intent, not evidence that server-side NPC removal already happened |
| Kick mutation | Original208 execute `0x1254c`, kicked flag true: clears NPC membership/name/badge and starts its native donation cooldown |
| Kick response | 24307 contains the fixed BE removed ID; the native UI updates through its original handler |

The client still contains standalone KickAllianceMemberMessage14307. A bounded
526 parser stages equivalent ID-first bytes and uses that original decoder for
the common member controller. Only after the entire turn/trailer validates can
the kick execute. Duplicate kicks in one bundle and kick+matchmaking in one
bundle reject before kick dispatch. No generic success acknowledgement or
unimplemented-command suppression was added.

## Persistence and authority

Both native role changes in a leadership handoff commit with both avatars in
the existing single-file fsync/rename. NPC mutation executes on an independent
original LogicGameMode; human mutation uses its actual current home game mode.
Original210 changes roles, and original208 performs removal. No human/NPC
currency, XP, deck or inventory is reset or filled. The ordinary guest excludes
the private intentional-crash probes used for qualification.

GDN1 **version2** has ten LE u32 words (40 bytes): tag, version2, clan serial,
native NPC body size, avatar offset, accounted epoch, human-body FNV, NPC-body
FNV, present0/1, header FNV over36 bytes. The native NPC home/avatar follows.
Version1's nine-word/36-byte header remains readable and means present1;
conversion writes only after all native/extension validation succeeds.

A kick sets present0 but retains the original serialized actor and donation
history. The original native avatar has no clan afterward; after decode its
unused role can default to NonMember. Roster/search counts use **present**, not
body size. Donation simulation stops for retired actors. Login and settings
cannot rejoin/reseed them. Retained profiles can still describe the actual
unaffiliated actor; no vanished or fabricated online member is advertised.

The local topology permits human Leader2 with NPC Member1/Elder3/Co-leader4,
or human Co-leader4 with NPC Leader2. A retired NPC does not lead. Invalid
two-leader/no-leader states reject before migrations or timer snapshots.
Historical chat/request/friendly authors retain their original stored role;
new entries encode the actual current human role, including Co-leader4.

GCL1 retains its ten-word layout but its operation word now uses the low byte
for status0..4 and bit `0x100` for **required donor record**. That flag is valid
only for active/leave-pending/settings-pending statuses2..4. Unknown flags or
invalid statuses reject. Every new checkpoint with an NPC body, including a
tombstone, sets the flag. A missing GDN1 then fails at load instead of looking
like an older clan and receiving new stock. Whole-clan disband clears both
the donor and flag in the same commit. Old unmarked saves remain distinguishable
only to the extent their old format allowed; the new flag protects subsequent
checkpoints. It does not retroactively authenticate an older file.

**Do not downgrade a migrated instance.** Older guests reject GDN1 version2 or
the required-donor flag; keep the preserved earned/failing sources intact.
An exact already-committed role/removal request can receive its matching
response again without repeating mutations. A new unauthorized role change
cannot gain that exception; original rank permissions and topology apply.

## Actual-client qualification

All cases below are labeled synthetic King-3 clan components, not earned
King-3 progression or a new iPhone qualification. The original earned King-2
save and phone-failing instance were not reset or replaced.

- Unchanged127ad31 reproduces the Promote abort at outgoing14306:
  `/var/tmp/hvm-clash-role-baseline-confirm`.
- `/var/tmp/hvm-clash-member-promote-elder`, `member-elder-return`,
  `member-promote-co` and `member-promote-leader` (all prefixed
  `/var/tmp/hvm-clash-`) qualify actual confirmations and persisted
  1→3→4→2 NPC roles. Leadership ends human4/NPC2, with exactly one leader.
- `/var/tmp/hvm-clash-member-demote-elder` and `member-demote-member` qualify
  actual4→3→1 on an independent copy taken before leadership handoff. These
  are separate QA branches, not a save rollback presented as recovery.
- The initial Kick candidate discovers unported **526**, not a standalone
  kick message: `/var/tmp/hvm-clash-member-kick`. Its same failed instance then
  completes native removal with the validated turn decoder in
  `/var/tmp/hvm-clash-member-kick-after-turn-fix`. No assertion was hidden.
- `/var/tmp/hvm-clash-member-co-chat` shows a new original chat entry labeled
  Co-leader, alongside historical Leader entries. Saved human/NPC roles4/2,
  human Gold2940/gems87/XP6 and original cards/trophies remain intact.
- `/var/tmp/hvm-clash-member-co-request` qualifies an actual new command524
  after handoff. Its original entry is labeled Co-leader, request2 stays0/10
  against exhausted NPC stock, and the source's historical Leader entries are
  retained. Human/NPC roles4/2 and complete economy are unchanged.
- `/var/tmp/hvm-clash-member-leader-return` uses the final version2/parent-flag
  candidate: original NPC Leader profile, appropriately absent Kick/Promote
  controls for the lower-ranked human, and saved GCL1 operation word0x102.
- `/var/tmp/hvm-clash-member-retired-request` relaunches the kicked instance
  and makes an actual new request2. Online1/1 and request0/10 remain; NPC does
  not return/refill. Human Arrows2, NPC0, human Gold2940, NPC105/XP1 persist.
- `/var/tmp/hvm-clash-member-corruption/summary.json` records **16** actual
  damaged-file cases, each exit70 with the failing bytes retained. They cover
  invalid presence/version/header/body/clan, duplicate/truncated/missing child,
  no leader/two leaders/two co-leaders, out-of-range NPC role, falsely present
  kicked actor, the **entire retired child removed after an unfulfilled request**,
  and an invalid legacy-v1 topology before migration. The frozen test sources
  remain byte-identical. This is corruption detection, not cryptographic auth.

The authored suite passes **25** tests in `/var/tmp/clan-controls-v2-tests.log`:
native 526 staging/full-turn validation, prefix truncations/oversized text/
bad trailer/duplicate/mixed matchmaking with zero dispatches, one-leader
invariants, GDN1-v1/v2 storage corruption and presence, GCL1 required-child flags,
Co-leader stream metadata and all four freestanding ARMv7 build flags with the
expected six original trampolines and no new imports.

`/var/tmp/hvm-clash-member-crash-qualification/summary.json` records four
deliberate abort-injection cases: leadership before/after commit and kick
before/after commit. Each file is relaunched twice with the normal candidate.
Before commit the old roles/membership remain; after commit the new state
remains. Leadership is2/4 before or4/2 after (human/NPC); kick is joined before
or retired afterward. Both actors' full card maps, resources, XP, gems,
trophies and native NPC counters remain unchanged. There is no replay, extra
charge, duplicate donation or rejoin. Pre-commit UI intents are uncommitted and
do not automatically replay on restart. These are explicit aborts, **not**
SIGKILL, power-loss or iPhone tests. Fault-injection sources/packages stay private
outside Git; normal builds contain no injection. Frozen source copies remain
untouched. The pre-leadership fixture was reached by actual1→3→4 confirmations
in `/var/tmp/hvm-clash-member-crash-prepare-{elder,co}`.

Qualified default package: `/var/tmp/hvm-clash-clan-controls-v2.ipa`, SHA-256
`fd6ec88c2c7048e3c0d9d6321345c003471222b1df39a923c0c331bec4005542`;
executable `e15daaf7680b8ff518dd7908d7a23483352628bec9478adee0d5704208a6baf3`.
Original assets are unchanged; the guest still needs no host Clash backend or
network. Its actual leader/retired returns and all damaged-file trials use this
candidate. Earlier named member-controls/final/turn candidates are component
investigation artifacts, not this final package.

Committed UI plans: `clan_member_promote_ui_plan.json`,
`clan_member_demote_ui_plan.json`, `clan_member_kick_ui_plan.json`, plus the
existing donor profile/chat/request plans in `tools/clash_guest/`. Observe rank
preconditions, use a private QA copy for mutations and allow startup to settle.
Capture alone is not a visual pass; inspect native dialogs/labels and native
state, then relaunch the exact same sandbox with an extracted .app.

Shared `src/` is unchanged from the qualified912a87e runtime (tree
`9b29155c175d76eda08111072700403272a19d7e`). Reuse its recorded260/260 runtime,
18/18 TestLab and zero-regression compatibility evidence; these guest-only
checks do not constitute a new16KB iPhone pass. Nonzero PCM/zero mixer drops
is audio-path evidence, not a fidelity or stutter verdict.

While this component was being qualified, origin advanced with `7bafe2a`
(CI toolchain timeout/retry only). It was fast-forwarded without replacing
any guest work. It changes no tested guest/runtime source.

Next: reciprocal NPC requests/player donations, explicit invitations/rejoin with
retained stock, real stock supply and weekly statistics. Continue the owner's
post-training names, legal varied decks/native styles, original daily shop,
remaining arenas and complete visual/audio/control/performance audit afterward.
