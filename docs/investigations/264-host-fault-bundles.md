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
