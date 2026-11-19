# iOS behavior contracts — 2026-11-16

Owner handoff item 2, issue #487. `IOSConformanceTests.cpp` adds a focused
test executable and runs the same cases from `hvm_tests fulltest`. Each group
cites its public Apple API contract, historical libmalloc source, or recorded
game observation. No Apple implementation code is copied or vendored.

The 46 checks cover 19 explicitly enumerated 32-bit allocator boundaries,
malloc_size/alignment, calloc zeroing, realloc capacity/prefix/failure rules,
NSLock/NSRecursiveLock, recursive/nil @synchronized, NSCondition timeout,
discarded pre-wait signals, signal with lock contenders, broadcast, cross-thread
performSelector delivery, empty run-loop return, mode-specific and common-mode
timers, port-only waits/removal, file/directory/absent paths, NSNumberFormatter
NoStyle, a valid current locale identifier, and default offline reachability.
Existing fulltest pthread signal/broadcast/timed-wait cases remain unchanged.

The allocator table is historical **32-bit** scalable-zone behavior, not a
promise about modern nano malloc or 16 KB iPhone backing. Tests do not insist
that realloc beyond capacity move or stay; both are legal. Locale defaults are
user/device-dependent, so tests do not pretend all iPhones use en_US. The
current fixed-locale approximation and locale-specific formatting beyond that
subset remain separate implementation work. No device locale oracle was
recorded in this task.

New standalone suite: **46 passed / 0 failed** on baseline `7fdc8a18`, Linux
x86_64 / WSL2 / Clang 18 Release / 4096-byte host pages. No new runtime
divergence was demonstrated, so this PR contains tests only and does not
claim a runtime fix. No phone run or source licensing change is implied.
Integrated validation: required targets build; **847 fulltest / 0 failed**;
**411 tools tests OK, 4 skipped**; stub audit **0 new/changed** (664 legacy).

Run:

```sh
ninja -C build hvm hvm_tests hvm_support_tests hvm_ios_conformance_tests
build/hvm_ios_conformance_tests
build/hvm_tests fulltest
```
