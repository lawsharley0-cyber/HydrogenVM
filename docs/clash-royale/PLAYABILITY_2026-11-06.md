# Offline playability continuation — November 6, 2026

Later supplement: [DRAW_CHECKPOINTS.md](DRAW_CHECKPOINTS.md) records the new scoped draw-save path and corrected battle-avatar wallet source. Active draft PR #366 carries the continuation; the original all-arena evidence below remains scoped to `3cb45ef`.

Continuation from `29912d9f81624e36e774ed190f9312734b5da62c`, branch
`codex/clash-offline-continuation`. PR #138 was found closed and unmerged on
resuming; do not describe its work as merged into main. A separate checkout
was recovered at `C:/Users/Lawson_PC/Documents/Codex/2026-11-06/h/work/HydrogenVM-offline`.
The unrelated `HydrogenVM` checkout belongs to other work and was not edited.

## Scope and native data

The guest previously rejected paid entry beyond Arena 2. It now generates
content and permits paid entry in **all six arenas of this pinned 2.1306.1
client**. Native CSV thresholds, demotion, locations, towers, entry prices,
battle simulation, score updates and rewards remain authoritative. This is
not yet a complete earned climb, victory/reward or every-button qualification.

Profiles preserve the original first 36 indices, then append later arenas.
GBO1 layout/version remains unchanged; its valid generated range expands to
78. The bounded choice buffer expands from 64 to 128, retaining range checks.
Profiles are grouped using non-training original NPC trophy anchors within
the corresponding native arena's promotion range. NPC mission, deck index,
arena and tower level are exported from the validated policy; the independent
C++ fixture exporter still reads the native client files and verifies levels.

| Arena | Promotion | Demotion below | Entry Gold | Profiles | Native NPC King |
|---|---:|---:|---:|---:|---:|
| 1 | earned training | 0 | 1 | 18 | 2 |
| 2 | 400 | 350 | 2 | 18 | 3 |
| 3 | 800 | 750 | 3 | 12 | 4 |
| 4 | 1000 | 950 | 4 | 12 | 5 |
| 5 | 1200 | 1150 | 5 | 12 | 6 |
| 6 | 1400 | 1350 | 6 | 6 | 7 |

Each arena has six eight-card authored deck variants. Later pools introduce
legal native Barbarians/buildings, Hog Rider/Minion Horde, Wizard/Zap, and
Mortar/Elixir Collector options through explicit substitutions. Source checks
reject duplicates, unknown/future unlocks and costs outside the existing
six-elixir affordability bound. This does not claim every original card is
represented: expensive Pekka/Golem and Mirror are not in these chosen bot decks.
The existing common-2 / rare-epic-1 card-level policy remains; later difficulty
and deck balance are unqualified. Native decision logic and bounded reserve/
mana behavior are unchanged. All seven original trainers remain unchanged.

## Exact qualification and reproduction

Pinned source archive SHA256 remains
`29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`.
Build with:

```sh
python3 tools/build_clash_guest.py /var/tmp/clash-client.ipa \
  /var/tmp/hvm-clash-allarenas-v2.ipa --build-dir /var/tmp/hvm-clash-allarenas-v2-build
python3 -m unittest discover -s tools/tests -p '*clash_guest*.py' -v
```

Use fresh output paths: the builder intentionally refuses existing outputs.
The second command passes **40 focused builder/guest tests**. New authored
fixtures check all six pools, exact 18/18/12/12/12/6 counts, preserved first-36
indices, original tower levels, invalid unlock/arena/tower data, selection
through six-arena promotion/demotion and new-bound GBO1 roundtrips. One negative
test initially exposed a KeyError for a missing unlock arena; the builder now
rejects that input explicitly with ValueError. No game assertion is suppressed.

The private C sector verifier matches the independent C++ exporter for **78
arena templates plus seven original trainers**, 85 byte comparisons for the
selected earned deck/King-2 analysis fixture. This is not an exhaustive
all-level/all-deck render matrix. As before, only that disposable analysis copy
clears the two native UI card flags absent from the oracle's Card model.

Actual-client runs use unchanged qualified host
`/var/tmp/hvm-clan-core-work/build/hvm` (source `912a87e`, shared src tree
`9b29155c175d76eda08111072700403272a19d7e`). No shared `src/` changes in this
stage. Prior 260/260 fulltest, 18/18 TestLab and the existing indexed regression
report remain the host qualification; they were not newly repeated here.

Private procedures are `outputs/playability_qa.py`, `playability_oracle.py`
and `native_inspect.py` in the new task directory. Captures/logs/checkpoints
are under `/var/tmp/hvm-clash-playability-20261106`. `check_legacy_profiles.py`
also compares all first 36 outputs byte for byte with the previous verified
Arena-2 exporter. All 36 match. Extract the candidate and
use `tools/clash_guest_ui_probe.py --app <bundle> --home <isolated-copy> --gpu
--capture-audio --startup-seconds 20 --seconds 240` with the recorded plans.
Never import/reset an IPA over the preserved owner instance.

The native-client tests copy `/var/tmp/hvm-clash-shop-qa/earned`, then stage
only the trophy input, repairing body/extension bindings. **These staged trophy
fixtures are not earned progression or grants to the owner.** Gold begins 75,
Gems 87, XP 8, King 2, earned wins 9 and losses 8. Original source SHA256 is
checked unchanged after each test. Each scenario closes the native arena panel
at `(160,540)`, enters at `(160,365)`, waits for real unassisted combat and
defeat, taps original result OK `(160,500)`, then independently relaunches.
Inspect screenshots, native fields and logs together; captured count alone is
not a pass. The arena-specific native entry charge occurs once, losses rise
8→9, wins remain 9, and Gems/XP remain unchanged. The exact saved outcome is
retained on relaunch without another charge.

Arena 3: original scene/King 4, Gold 75→72, trophies 800→776, profile 38.
Arena 4: original scene/King 5, Gold 75→71, trophies 1000→970, profile 50.
Arena 5: original scene/King 6, Gold 75→70, trophies 1200→1170, profile 62.
Arena 6: original scene/King 7, Gold 75→69, trophies 1400→1370, profile 74.
All four actual defeats and independent relaunches pass, retaining exact native
resource/counter/history values. Their result screenshots show original OK and
the selected handle. All four mixed recordings contain nonzero PCM samples.
Fresh original Trainer Earl/tutorial and
the existing local-clan conversation return smoke also run on the candidate.
Clan smoke is not a repeat qualification of every role/donation/mail control.

Candidate IPA SHA256:
`69316de199b15e5a51c7978b3e6ec1f192f2f85217a6324558c89b6df924931f`;
executable:
`2604c36d5e9b8a2cbfd081a915d478924f3ab4bda06c503ddb3e426fa6536251`.
The final unknown-unlock error diagnostic and explanatory comments do not
change generated valid profiles or the runtime payload. Private binaries,
fixtures, authored diagnostic saves and native client assets remain outside Git.
New later-arena indices require this newer guest; do not downgrade their saves
to an earlier 18/36-profile guest. Audio capture is enabled, but this establishes
no iPhone, 16 KB host, audio-dropout, stutter or visual-fidelity verdict.

## Next work and owner priorities

Qualify earned arena transitions/wins/chest rewards and draws/partial-crown
losses/profile accounting. Disassembly reconfirms native winner field at
Battle+0x74; draw is -1, and original win/loss setters are skipped. Current
guest saving still depends on those setters, so partial-crown draws need a
dedicated completion path and targeted native tests. Do not mark that fixed
from zero-crown defeats. Original getWinnerAvatar/getLoserAvatar addresses are
0x14088/0x140b4; native battleEnded ends through 0x153f4 without destroying the
result objects. See the retained native inspection output when implementing.

Then implement [bounded saved clan events](CLAN_EVENTS_PLAN.md), finish every
button/save-lifecycle audit, and resolve the owner's princess/cannon/health-bar/
scenery/text/audio/performance reports on the relevant device build. Advanced
battle-AI refinements remain deferred. The owner requested **modding afterward**:
record it as a later optional phase after reliable playability and bug fixes,
not a reason to postpone restoration or alter original training now.
