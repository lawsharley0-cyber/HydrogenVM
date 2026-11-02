# Host-fault evidence for #264

This work improves diagnostics; it does not establish the cause of Pocket Ants
#64/#264, Goat Simulator #125/#263, or the Pocket Ants hang #28. A synthetic
Linux fault is not a physical-iPhone reproduction.

## Regression

`hvm_host_fault_tests` runs an exec-isolated child, parks two guest threads in a
named HLE function, publishes 205 numbered log lines, then makes a native host
callback write to an uncommitted address in the guest arena. The parent checks
the fatal signal, both guest register snapshots, HLE name, arena offset and the
complete final 200 lines (over 40 KB). It uses a regular file so pipe capacity
cannot deadlock the test. Expected register values and offsets are chosen by the
test, independently of emulator output.

On unmodified main `4c09fa47`, the callback arena-offset and HLE-name checks fail;
the other four checks pass. The baseline report does not identify the active
arena on a callback thread without a guest fault scope.

The fulltest regression also faults during concurrent log writes and repeated
Cpu/Engine construction/destruction. It checks that the reporter still reaches
its footer and retains the two blocked threads' register/HLE observations. The
16 KB tests verify shared-page preservation, independent guest protection,
partial unmap, released backing-byte accounting and zero-filled remapping.

An additional minimized failure escapes `Engine::run` via an HLE C++ exception,
then faults on the same host thread. Without exception-safe scope restoration,
the old thread-local pointer refers to an abandoned stack frame. The subprocess
still dies but its address/offset and report footer checks fail. Restoring the
scope with RAII makes all three checks pass; arena classification uses a copied
integer base so it cannot dereference an arena object during teardown.

## Report limits

The reporter uses permanent storage for 128 CPU slots and 32 arena bases.
Numeric register fields are independent samples at block entry/HLE boundaries,
not an instruction-precise, simultaneous stop of all host threads. Each thread
record includes its arena and a slot generation; a reused slot is discarded if
its generation changes during capture. HLE names are bounded to 127 bytes.
The last 200 log entries are bounded to 255 bytes each. A signal interrupting a
publisher never waits on it; unavailable HLE/log data and CPU-capacity overflow
are reported explicitly. A 16 KB Linux pass does not prove device equivalence.

Batch journal identity is saved before the native signal files open. Old
nonempty evidence must be recovered successfully before those paths can be
truncated again. A run whose arena has not released keeps its journal open;
session tokens stop an older task's cleanup from deleting a newer journal.
Recovery tolerates missing pending metadata and invalid UTF-8 in a clipped log
line. No proprietary fixture or actual device crash is included in this patch.

## Verification record (2026-11-01)

- Linux x86_64, native host pages 4096. Baseline main `4c09fa47`: fulltest
  487/487; Test Lab 25/25, zero failures, interruptions or skips.
- Final focused ASan/UBSan: forced fault, concurrent teardown/logging and 16 KB
  checks pass. `bash tools/test_host_fault.sh` leaves normal memory/UB
  instrumentation enabled; only sanitizer interception of the deliberately
  fatal SIGSEGV/SIGBUS is disabled so the emulator's real handler runs.
- Final Test Lab: 25/25, zero failures, interruptions or skips.
- The first final release fulltest aborted with `double free or corruption
  (fasttop)` immediately after the formatting regression and before the asset
  I/O results. Four subsequent fulltest runs passed 488/488. The abort remains
  an unresolved gate concern. Eight further GDB runs exited normally without
  reproducing the abort; that is not a fix. After review changes, release
  fulltest again passed 488/488 and the focused ASan/UBSan tests passed,
  including the exception-unwind regression.
- The full runtime was built under ASan/UBSan. Its fulltest stops before the
  heap-abort location at unchanged `src/runtime/graphics/Shader.cpp:1455:29`:
  `2.14748e+09 is outside the range of representable values of type 'int'`.
  This gate failed; it was not suppressed. Evidence is posted on umbrella #234.
- Enabling `HVM_TESTLAB_TOOLCHAIN=1` exposed an existing test fixture assigning
  a CompletedProcess to `TestCase.run`. The result attribute is renamed to
  `testlab_run`; all original guest expectations remain intact. The enabled
  tools suite then passed all 202 tests with zero skips.
- CI stub audit reports four new/changed findings relative to its committed
  baseline, all in unchanged `RunLoop.cpp`, `Libcxx.cpp`, and `Pthread.cpp`.
  The baseline and the stub implementations are not modified here. The runtime
  tree is identical to main: `cd9f698d086bf7df9b168f8328a9bb5d48951571`.
- CodeRabbit's three findings were corrected: cleanup also runs from each
  attempt's exit callback after cancellation; malformed pending/signal bytes
  are retained as raw evidence rather than blocking future launches; each
  thread's expected LR/PC/block/HLE is checked on one complete record.
- Physical iPhone/game runs and local Apple compilation: NOT RUN. The PR stays
  draft; synthetic diagnostics do not resolve the underlying game faults.

## Owner capture on a physical iPhone

1. Install the CI unsigned-device artifact after signing privately. Record the
   actual installed HydrogenVM commit/build, device model, iOS version, host
   page size, renderer choice, game version and SHA-256 of the private IPA.
   Keep the IPA and its save files in `HVM_FIXTURE_ROOT` or the private store.
2. Preserve the current sandbox. Run Pocket Ants manually from launch through
   the reported ten-second freeze/crash; record the exact taps and elapsed
   time. Repeat in a batch session to exercise the separate journal path.
   A short launch or a different fixture is not resolution evidence.
3. After a host crash, reopen HydrogenVM before starting another game. Export
   the recovered full crash JSON using Share, including `hostFaultDiagnostics`.
   Retrieve the matching `Documents/Logs` session log. If no recovered report
   appears, retrieve `Documents/HydrogenVM/crashes/pending.json`,
   `pending.signal.json` and `pending.hostfault.txt` before any new launch.
4. Export the iOS Analytics `.ips` crash report (or Xcode device crash report)
   and the build's matching dSYM. Record exception type/subtype, fault address,
   crashed host thread and all host thread stacks. Keep these artifacts private;
   post only a scrubbed report and private artifact references on #264.
5. Verify the report contains its header/footer, faulting host address, active
   arena base and in-arena offset (or explicit outside-arena classification),
   each recorded guest thread's PC/LR/block and current HLE name, and recent
   log lines. Capture both software and GPU renderer results separately when
   checking the Cocos2d worker-GL hypothesis.
6. For Goat Simulator, enter the world, record the world-load action and run
   beyond 219 seconds. Capture the same artifacts at the first failure; attach
   them to #263/#125 without claiming a Pocket Ants root cause applies.
7. If Pocket Ants freezes while the host stays alive, retain the session log,
   record per-thread guest instruction deltas over time, then capture thread
   PCs and native stacks while frozen. Zero deltas require identifying the
   blocking mutex/condition variable; a fatal-signal bundle alone cannot do that.

Linux 16 KB commit-granularity emulation covers mapping bookkeeping and data
isolation. It does not emulate Darwin VM behavior, Metal/OpenGL driver calls,
device scheduling or the actual device fault.

## GitHub Actions provenance

Head ccc309fc84c5baf3428b2f650ec235a04e764a2e against main
4c09fa4708fe48796f3255695e97aef685087042 was tested as GitHub's PR merge
checkout 4dd2cb59aafc762942fbd0db4c2ea20f7490fb90 (verified in both job
checkout logs). [CI run 41358248915](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41358248915)
passed its Linux fulltest/performance and focused host-fault ASan/UBSan steps.
[Test Lab run 41358248896](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41358248896)
passed. The 202-test full tools result above is local, not a claim about those
Actions runs: CI previously ran only selected Python modules. This follow-up
adds the complete enabled tools suite and its jsonschema dependency to CI;
its actual result must be linked after that run finishes.

[Apple file-editor run 41358248921](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41358248921)
passed Swift tests and unsigned simulator compilation but failed the save/close
UI exercise. That is a failed run, not a physical-device or full Apple pass.

Head 57963f62d9d2e475f79416668c61337899624eca was checked out by
[CI 41364337611](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41364337611)
as merge 7cc032dd794be130801692fcbba129803aa405b3. The job log confirms
202 tools tests passed in 16.663 seconds with the integration enabled, plus
fulltest and focused ASan/UBSan success. [Test Lab 41364337427](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/41364337427)
passed. These are actual Actions results for that head, not the later follow-up.

Further review regressions exposed stale diagnostic inGuest=true after an
outermost C++ unwind. The new test failed against the preceding candidate,
then passed with the previous diagnostic sample restored (false outermost).
The signal regression now passes mode independently and deliberately uses
child descriptor zero, checking an unwind-only log marker. Neither test can
accidentally pass by running the ordinary fault path.

Three actual-app XCTest cases cover clipped/invalid pending and signal bytes,
signal evidence without pending metadata, and a real ENAMETOOLONG save
failure retaining all journal bytes before a successful retry. They use the
real engine bridge in an isolated app test sandbox, without success stubs.
Their Apple Actions result remains pending; no local Swift pass is claimed.
The earlier file-editor failure included missing bridge types and BinaryEditorView
in its separate synthetic UI harness. The actual shell compilation passed;
the new journal test target does not depend on that harness.

The review C++ candidate again passed local fulltest 488/488, Test Lab 25/25
zero skips, the complete enabled tools suite 202/202 and focused ASan/UBSan.
The Apple journal gate first requires the three named recovery tests to fail
against actual earlier CrashReport.swift from 1a874239966f0f04697aaaa2897103a094846e6d,
then restores the candidate and requires the complete journal tests to pass.
A baseline build failure cannot satisfy the required named-test failures.
The test source and actual bridge stay identical between both executions.

The failed-save fixture uses a valid overlong bundle identifier so report-file
creation fails with ENAMETOOLONG while the three pending paths remain writable
and deletable. A read-only-directory fixture would also prevent deletion and
could hide the old unconditional-cleanup bug. The named failed-save test is
therefore also required to fail against the earlier actual recovery source.
After checking the exact retained bytes, the test fixes only its synthetic
metadata and verifies successful recovery; production saved state is untouched.
