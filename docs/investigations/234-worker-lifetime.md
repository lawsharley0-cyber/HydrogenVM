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
