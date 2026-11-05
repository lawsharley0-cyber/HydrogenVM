1. **Analyze Reviewer Feedback**
   - The reviewer explicitly stated: "Or, simpler and preferred: drop the new `pthread_attr_setstack`, `getstack`, `setstackaddr` and `getstackaddr` support from this PR entirely. Issue 314 is about default stack size and guard fault handling, not custom stack addresses."
   - We must ensure setters like `pthread_attr_setstacksize` return `EINVAL` (22) for invalid sizes (below minimum).
   - The race condition in the test (where the worker reads its thread ID before `pthread_create` writes it) needs to be fixed. We should pass the information through the thread argument or synchronize.
   - The test must assert that the worker finished (`bprobe->done`).
   - The test for `NSThread` needs to verify the *real* allocated stack size of the started thread, not just the `stackSize` property (which just reflects the set value).
   - "The test probes addr minus size and addr minus 1 outside a fault-catching scope. Do the guard probe only inside the existing FaultScope or CPU GuestFault path, and assert that the fault was caught."

2. **Update `src/runtime/libc/Pthread.cpp`**
   - Only modify `pa_setstacksize` to enforce minimum stack size constraints. POSIX minimum stack size is `PTHREAD_STACK_MIN`, which is generally `16384` (16 KiB). The `Process::startThread` already caps it: `std::max<uint32_t>(uint32_t(rounded), kMinThreadStackSize)`. However, `pthread_attr_setstacksize` itself should reject small sizes (return `EINVAL` = 22). Let's check `kMinThreadStackSize`. It's `16u << 10`. So `pa_setstacksize` should return `22` if size < 16KB.
   - Update `t_create` to properly read the `stacksize` and pass it to `startThread`. But `t_create` already does this! Wait, the issue says "pthread_create honours the attr stack size only when the attr carries the THDA signature; default is 512 KB". Let's check `t_create` in `Pthread.cpp`.
     ```cpp
     uint32_t stack = 512u << 10;
     if (attr && x.mem.read<uint32_t>(attr) == 0x54484441) {
         stack = x.mem.read<uint32_t>(attr + 4);
     }
     ```
     This actually seems correct, but maybe the game provides an attr *without* `0x54484441` signature?
     If `pthread_attr_init` is called, it sets the signature. If the game doesn't call it, it won't have the signature. But we shouldn't change the signature requirement. Wait, if `pa_setstacksize` returned 0, it means it succeeded. If the game passed an uninitialized `attr`, `pa_setstacksize` would still write to it and return 0! So the bug is that `pa_setstacksize` doesn't check the signature before writing, but `t_create` DOES check the signature before reading! So an uninitialized attr gets successfully mutated but silently ignored.
     Let's update `pa_setstacksize`, `pa_getstacksize`, `pa_setdetachstate`, `pa_getdetachstate` to return `EINVAL` (22) if the signature is missing.

3. **Update Test in `tests/runner/TestRunner.cpp`**
   - Write the test to fail first on unmodified `main`. Run tests and capture failure.
   - The test must use `NSThread` and `pthread_create` to verify 1MB stack size bounds, and must explicitly catch `CPU::GuestFault` when accessing `addr - size - 1`.

4. **Run tests**
   - Verify the test fails on `main`.
   - Apply fixes to `Pthread.cpp`.
   - Re-run the tests to verify they pass.

5. **Commit and Submit**
   - Open a Draft PR, following all constraints.
