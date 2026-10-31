# Memory investigation follow-up

## Apple CI correction

The exact-candidate push run [40582883387](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40582883387),
commit `f63368e`, passed Linux but failed the Apple C++ build before device or
simulator packaging. Its published report identifies
`os_proc_available_memory is unavailable: not available on macOS`.
The PR-triggered run [40582955725](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/40582955725)
passed Linux and skipped macOS, which is why its overall success was insufficient.

The correction includes TargetConditionals and guards the iOS-only header/call
with `TARGET_OS_IPHONE`. Mach task resident/physical footprint remains measured
on macOS and iOS; available process headroom remains null on macOS. Linux is
unchanged. This requires a new exact-commit Apple CI result; no Apple pass is
inferred from Linux tests. Before correction: fulltest 271 and Test Lab 18 pass.
After correction: fulltest 271, Test Lab 18, memory tests 12 pass; full report
build 1 / CPU 15 / iOS guests 6 / bench 5 passes. The CoreMark notice remains
informational; compiler warnings are recorded, not silently removed.

## Fixture recovery and isolated guest-source checks

Recovered the documented original client into
`/workspace/library-files/clash-memory/original-2.1306.1.ipa`, outside Git.
Archive size 90,361,183 bytes and SHA-256
`29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f`
match the repository pin. No decryption or source/asset modification occurred.

Read-only guest snapshot: PR #138 head
`29912d9f81624e36e774ed190f9312734b5da62c`, extracted under
`/workspace/scratch/hvm-clash-source-29912d9`. Its **38 guest-source tests pass**.
No PR #138 branch or shared rendering/shell files were changed.

Built the default guest with its existing builder and original pinned input:

```sh
python3 tools/build_clash_guest.py /workspace/library-files/clash-memory/original-2.1306.1.ipa \
  /workspace/library-files/clash-memory/guestdev-29912d9.ipa \
  --build-dir /workspace/library-files/clash-memory/guest-build-29912d9
```

Output archive SHA-256
`10c303627c7ca6ed4496b8fd6482d21ddcd7bbbe5796c1388ae07f8e95bfb1ff`;
modified executable SHA-256
`c646a0050687b5cb9c9d9c5d85d2f285a8764d6e44fbe78b5fac339f5a1c516b`.
The builder verifies unchanged assets. This is the latest fetched guest source,
not a claim to reproduce the unidentified guest build installed on the phone.
No earned Arena-1 save has been recovered; do not manufacture progression and
call it an earned scenario.

## Attempt 1: preserve the failed cold launch

HostGL/Mesa, new private home, default guest, runtime based on this diagnostic
branch, no keep-going or assertion suppression. The game loaded original assets
and local login responses, then stopped at ~39 seconds / loading 85%:
M4A music load failed, followed by unimplemented `backtrace_symbols_fd` in the
client's diagnostic path. Exit 70; gameplay not reached. The local build had
`FFMPEG_FOUND` unset, so supported host-decoder dependencies are being enabled
before a separate attempt. This is not attributed to the iPhone incident.

Sampled RSS at stop: 412,999,680 bytes; live guest heap 13,399,664 bytes;
committed heap 25,165,824 bytes; mapped guest memory 38,400,000 bytes;
23,324 cumulative decoded blocks / 187,127 emitted ops; zero bad frees.
The startup rise is not a sustained-gameplay growth measurement.
Private log, screenshot and JSON remain in
`/workspace/hvm-cloud/artifacts/memory-clash-fresh/`. This attempt must remain
recorded even if a later environment-corrected run succeeds.

## Phone baseline collection

The installed HydrogenVM build can provide baseline evidence without an update.
Record its displayed commit/build and the Clash guest build if available.
Keep the current save; do not uninstall, reset progress or import over it.
Runtime memory samples (`[memory]` log lines and `runtimeMemorySamples`) are written
only when `HVM_MEMORY_DIAG` is set (to any value) in the launching environment, for example
`HVM_MEMORY_DIAG=1 ./hvm <app>` on a host. No iPhone shell setting for that variable
exists in this repository, so an already-installed phone build without it records only
its existing available-memory history; do not describe such a run as sampled.
Before gameplay, exit the guest and make an independent copy of the current
`Documents` and `Library` homes (Files → On My iPhone → HydrogenVM → copy to another
location). Verify the copy opens and has matching file counts, and keep it separate
from the live home so the pre-run state stays restorable.

1. Note the session start time and starting game state.
2. Leave home idle five minutes, then play normally for ten consecutive battles
   or 30–60 minutes. Note each result and return-home time.
3. Record pauses/backgrounding and any unexpected termination time.
4. Preserve the matching HydrogenVM run log in `Documents/Logs` (normally exposed
   through Files → On My iPhone → HydrogenVM), plus the interrupted-session
   report and any matching Settings → Privacy & Security → Analytics &
   Improvements → Analytics Data → JetsamEvent report.
5. Share those files for analysis. An older build may contain only available
   headroom, so it cannot replace the final instrumented-device qualification.

The physical device is user-operated; no phone run is claimed here.
