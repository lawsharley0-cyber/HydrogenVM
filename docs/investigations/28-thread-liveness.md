# #28: capture per-thread progress before diagnosing a freeze

Enable `--thread-liveness` on the CLI, or set `HVM_THREAD_LIVENESS=1` on the
host process (including an Xcode-launched device build). A cancellable sampler
records one observation per second independently of rendering and the main
guest thread. The report retains the last 120 observations in `threadLiveness`;
the `[threads]` log lines persist the whole run even if a final report is never
written. Neither a zero delta nor a missing sample is called a deadlock.

Every valid live-thread record includes a cumulative approximate guest
instruction count and delta, PC/LR/block at a dispatch/HLE boundary, current
HLE and its first two entry arguments. For `_pthread_cond_wait`, argument0 is
the guest condition-variable address and argument1 is its guest mutex. Mutex
waits expose the mutex as argument0. Frame, mixed-audio and audible-frame
counters are sampled on the same timeline. Unavailable register observations
are explicit; the sampler never reads a running CPU's plain register array.

CPU instruction counters are atomic. A single CPU owner publishes diagnostic
fields using a bounded atomic snapshot protocol; nested guest calls restore the
outer HLE arguments and budget baseline. Publication is opt-in. The focused
regression independently expects 40,000 then 50,000 retired instructions from
an ARM branch-to-self and validates a parked HLE's PC/LR/function and wait
arguments. Before implementation, that test cannot obtain a progress snapshot
from main. This is an observability regression, not proof of the game's cause.

The available private Pocket Ants app is an extracted `anthill.app`, bundle ID
`LKABS78UWF.com.concretesoftware.anthill`, executable SHA-256
`7b56022ea99d53b3c7ec4d1aed1d5dffe7dd36b105103f2b1e3b36fff936cf5f`.
The original archive is unavailable; do not claim a match to manifest IPA
`80b3df4bff0c783f5786faeba910e252c433c71b7650c85b3055a7b398a38cdf`.

Run at least 60 seconds with a private preserved home, `--thread-liveness`,
`--report`, `--frame-file`, and a recorded input sequence. Compare per-thread
instruction deltas with frame/audio deltas before and after ten seconds. If
instruction progress disappears, retain all thread records and acquire native
thread stacks while still frozen; match the HLE and argument addresses to the
pthread/condition-variable or GL operation before changing that primitive.
Keep software and GPU runs separate. On the device, export the full session log
and #264 host-fault bundle if it crashes; a Linux result does not close UAR-007.

Baseline main `4c09fa47` passed fulltest 487/487 and Test Lab 25/25 on Linux
x86_64, pageSize 4096. Final gates, the affected app trace, and any diagnosed
lock/cause must be recorded in the PR. No game-specific fix is included without
that evidence. WSL interrupted the initial build; validation resumed with two
compiler jobs after the instance recovered.

## Local validation and private trace

The final Linux candidate passes fulltest 488/488, Test Lab 25/25 with zero
skips, and all 202 tools tests with HVM_TESTLAB_TOOLCHAIN=1. A regression added
after the first build exposed a stale PC/LR after a nested guest call; it failed
before restoring and republishing the outer registers, then passed.

The 60-second private software trace remains at an OpenFeint opt-in screen,
not confirmed gameplay. Sixty-five progress samples were captured. Brief zero
deltas at 4/6/7 seconds show only the main thread in UIApplicationMain
(PC 0xffff4f10, LR 0x5e28), followed by about 35 guest instructions/second.
No pthread mutex or condition-variable wait is identified by those samples.
Only six frames were presented. Graceful termination starts two busy callback
threads and hits the five-second saving watchdog. This is neither a successful
quit nor evidence that the historical in-game freeze has been reproduced.
Keep the issue open and capture the same timeline after dismissing onboarding
and confirming game entry, on both renderers and the actual device.

Focused ASan/UBSan also passes, including nested-call restoration and parked wait arguments.
