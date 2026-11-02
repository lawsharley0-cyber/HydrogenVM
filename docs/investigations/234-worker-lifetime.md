# #234: native worker ownership and arena release

The previous destructor waited two seconds for `GuestThread::finished`, then
released the thread records, HLE functions and arena even if a native host
worker was still executing them. Even `finished == true` was insufficient:
the worker still notified its condition variable and cleared host TLS afterward.
Guest pthread detachment must not detach ownership of those host resources.

The regression holds a real worker inside an HLE callback past 2.3 seconds.
Against main `4c09fa4708fe48796f3255695e97aef685087042`, it printed:

```
[hvm:warn] Process teardown: guest threads still running 2 s after stop:
  thread 2: inside held_host_callback; last calls: held_host_callback
FAIL Process/arena destroyed while a host worker still executes its HLE
```

The failing child exits before releasing that worker, avoiding an intentional
use-after-free in the test itself. The patched expectation is that destruction
still owns the arena until the callback is released, then completes and allows
ten later Process arenas to reserve and release. The test also accesses a
surviving Process after cross-thread destruction and rejects new workers after
stop. It runs through exec, so a fulltest parent with other host threads never
constructs a Process between fork and exec.

The fix keeps native pthreads joinable and joins every owned host worker before
member destruction, including guest-detached workers and ones already marked
finished. Admission and handle publication are serialized with the thread
registry. It retains the two-second diagnostic warning without treating that
deadline as permission to free live state. A unique Process identity prevents
host TLS from dereferencing a thread record belonging to a destroyed Process.
Completed native workers are reaped on subsequent creation/join operations so
their 8 MB host stacks do not accumulate until Process destruction.

A private 420-second Goat probe exposed a separate reporting defect: the
watchdog recorded `crashed=true` while `--run-for` forced the process/report
status to zero. The CLI now gives a recorded crash precedence over timed-stop
success. The watchdog remains a failure; this does not claim its save completed.

This is lifetime safety, not a claim that every external host call can be
cancelled. An uninterruptible call keeps teardown pending and retains its arena;
the app must continue to contain that timeout instead of starting another game.
No state is reset and the existing call-ring implementation/tests are retained.

Baseline fulltest was 487/487 and Test Lab 25/25 on Linux x86_64, native pages
4096. The new failure above was reproduced with that baseline library before
the fix. Subsequent local validation was interrupted by an unresponsive WSL
instance; final build/fulltest/Test Lab/tools and ASan/UBSan results must come
from the linked draft PR's CI before readiness is considered.

The private fixtures now available for the four requested app probes include
the exact Angry Birds IPA and extracted Minecraft PE Lite, Talking Larry and
Mega Jump apps. The extracted apps have executable/tree fingerprints rather
than a verified original IPA hash. The four-game quit timings and a physical
iPhone ten-game batch remain unverified until the runner is responsive and the
owner provides device evidence. Do not close #234 based on this synthetic test.

The review follow-up also tests a guest that enters its actual
applicationWillTerminate callback and deliberately keeps executing guest
instructions until the saving watchdog fires. On main, both readiness markers
appear and the report says crashed=true, but process/report statuses are 0:

    FAIL termination watchdog must produce host/report status 70 and a recorded crash; actual 0 0 True

The unchanged five-second watchdog must instead yield process/report status
70. tools/test_shutdown_watchdog.py builds and drives that minimal guest; CI
runs it after the normal build. It does not simulate a successful game save.

## Completed Linux validation

Candidate runtime source 665580a029e7fe464aeb35b497876c85e2f02e27 passed
local fulltest 488/488, Test Lab25/25 with zero skips, complete enabled
tools202/202 with zero skips, and the real guest watchdog negative regression.
[Actions CI41364588649](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41364588649)
tested that head as merge eb26f8ea17ae44e08395137f5eade4c3dd5463bf. Its log
confirms the full-runtime ASan/UBSan worker-lifetime test, fulltest/performance,
202 tools tests and the actual shutdown-watchdog regression all passed.
[Test Lab41364588715](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41364588715)
also passed. Stub-honesty still fails its unchanged baseline findings.

Four private software probes ran10 seconds before graceful stop. All reported
exitStatus=0/crashed=false and saved/closed. Total process wall time minus the
ten-second run duration is a conservative upper bound including startup and
final arena destruction, not a precise UI stopwatch:

| Fixture | Shutdown upper bound | Presented frames |
|---|---:|---:|
| Exact Angry Birds IPA |1.170s|92|
| Extracted Minecraft PE Lite |0.837s|35|
| Extracted Talking Larry |0.311s|24|
| Extracted Mega Jump |0.362s|17|

Private evidence: teardown-four-apps-1793578417817425308 under
HVM_FIXTURE_ROOT/evidence. A separate Mega Jump batch completed all ten
launch banners in one host process, returned zero in27.743s and retained
the same home: teardown-ten-launches-1793578536836452643. It exercises
relaunch/arena reuse for one fixture, not ten different physical-device games.
The synthetic regression independently checks live arena counts across ten
subsequent Process lifetimes. No private screenshots/logs/saves are committed.

## Physical-device owner capture

Record full installed commit/build, renderer, device/iOS and measured native
page size; fingerprint each private IPA and preserve every home. Time the
close action until the library is usable and arena count is zero for Angry
Birds, Minecraft PE Lite, Talking Larry and Mega Jump. Retain session logs
and save bytes. Then run at least ten sequential game opens/closes in the
same HydrogenVM app process, recording the order, per-close duration, native
resident footprint and live arena count after each. A pending teardown must
retain its arena and contain the timeout instead of admitting another game.
For any failure export #264's recovered crash JSON/full log and matching
iOS .ips/dSYM, including all host stacks. A force-quit or OS restart cannot
count as arena release by the emulator. Keep #234 open until that device
sequence and the remaining umbrella children are qualified.
