# Offline continuation — October 23, 2026

Current owner-directed continuation is `codex/clash-offline-continuation`, based
on main `fdfee6f`. This supersedes the older README/TODO snapshot where it
describes missing host AAC or an unintegrated NPC peer. The owner reports the
current **iPhone** build renders the pinned 2.1306.1 startup at 60 FPS, with
music, then reaches connection retry. That report is not a local FPS measurement.

## Reproduced on the new Linux/WSL build

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
