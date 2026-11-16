# Scripted game smoke tests

Run from the repository root on a Linux bench with Python 3.10+ and Pillow
(`python3-pil` on Ubuntu) for comparisons:

```sh
export HVM_SMOKE_GAMES=/private/ipas
export HVM_SMOKE_REFS=/private/smoke-references
python3 -m tools.smoke.run --hvm build/hvm --out build/smoke
python3 -m unittest discover -s tools/tests -p test_smoke.py
```

Without `HVM_SMOKE_GAMES`, all games are **SKIPPED** with exit 0. Each missing
game is likewise skipped. `--only angry-birds-island` selects one scenario;
unknown IDs fail. `--games`, `--refs`, and `--scenarios` override paths. Outputs
get a unique `run-*` directory; existing evidence is never overwritten. Do not
commit or publicly upload these directories: reports/logs may contain private
paths or account data; screenshots contain game assets.

The four JSON files in `tools/smoke/scenarios/` contain bundle IDs, expected
versions/filenames, fresh-home setup notes, portrait-point taps, screenshot
times and provisional RSS ceilings. Put owner-supplied IPAs under the specified
filenames, or copy/edit manifests into a private scenario directory. `ipa` may
also name an extracted `.app` relative to the games root. The runner reads the
IPA's Info.plist without extracting it, verifies bundle ID and version, and
fails a present but wrong/corrupt fixture. A missing fixture is distinct from
a bad fixture. Minion Rush needs the **with-data** IPA: the existing importer
restores its `Container/` into the isolated home. Optional `home_seed` is a
relative private directory under the games root, copied for every replay;
missing seeds skip, and links/paths escaping the root fail. No user home is
modified, and game networking stays off.

For every screenshot time and the final duration the harness starts a separate
replay from the same initial home, schedules taps earlier than that endpoint,
and asks `hvm` for its last presented frame. This avoids depending on an
unimplemented timed-screenshot CLI flag. It is wall-clock replay, **not**
deterministic record/replay: timing may need qualification on a slower host.
The two Angry Birds checkpoints therefore cost 45 + 60 seconds. Homes are
deleted after each replay; run logs, runner JSON and screenshots are retained.

A run fails for nonzero process/runner status, load errors, a crash flag/report
or `Crash:` log, a `Stall:` log, early exit before the requested duration,
missing runner report, missing required log text, RSS over its ceiling, or a
wall-clock timeout (duration + 30 seconds by default). Linux `/proc` VmHWM is
sampled every 50 ms and combined with the runner's resident/peak memory
samples. Missing all RSS measurements fails. RSS limits are broad guardrails,
not qualified game baselines or a memory-growth oracle. `Snapshot:` at normal
shutdown and informational `Hitch:` lines alone do not fail a run. Missing API
taint and stub-hit totals are retained in the summary; smoke success does not
assert full API support or gameplay correctness.

References are **owner-reviewed images outside Git**, at each manifest's
`reference` path below `HVM_SMOKE_REFS`. Dimensions must match; RGB images are
downsampled to 64x64 and compared by mean absolute channel error normalized to
[0,1], default tolerance 0.04. A small antialiasing change can pass; black vs
white and dimension changes fail. Missing reference files mark the individual
comparison and the visual summary **SKIPPED**, while process checks can pass.
Use `--require-refs` to make missing references fail. A requested screenshot
that was never rendered always fails. The tool never blesses or updates a
reference from current output. Select references and tolerances independently;
a permissive threshold is not evidence of correct rendering.

Each scenario cites its evidence and has explicit TODOs where interaction
timings are not documented/qualified. Angry Birds menu timings are documented
in `docs/ANGRY_BIRDS_ISLAND_PLAYABILITY_2026-11-13.md`, despite the handoff's
earlier assumption that none existed. Groove currently asserts startup/data
update, not song play. Minion Rush uses a historical alert tap. Army of
Darkness uses the owner's approximate PLAY timing and verifies the documented
bundle ID against the private Info.plist. No game is newly declared playable
by this harness.

## Private CI

The owner may configure a trusted self-hosted Linux runner labelled
`hvm-smoke`, with the normal bench dependencies and Pillow installed. Set the
repository variable `HVM_SMOKE_ENABLED=true`, and repository secrets
`HVM_SMOKE_GAMES` and optionally `HVM_SMOKE_REFS` to **local directory paths**
on that runner. Dispatch `game-smoke.yml` on the branch to test. An absent
enable variable skips the job; an absent games path skips execution. No
public runner receives proprietary files, no pull-request event executes
untrusted code on this runner, and artifacts are not uploaded. Review the
branch before dispatching it. The synthetic harness unit tests remain part of
the ordinary tools suite on every PR.
