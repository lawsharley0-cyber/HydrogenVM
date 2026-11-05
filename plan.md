1. **Analyze Reviewer Feedback**
   - The test run failed. The error is `Process completed with exit code 1.`
   - In the logs: `regression test unit:fulltest: unit fulltest was pass and is now fail`.
   - The test I added to `TestRunner.cpp` failed on my latest push.

2. **Determine why the test failed**
   - I need to run `ninja -C build hvm_tests && ./build/hvm_tests fulltest | grep "pthread_attr_setstacksize"` locally to see the actual assertion failure.
   - Wait, `fulltest` might have failed because the `isExecuting` wait loop timed out, or because `faultCount` wasn't `1`, or because `ns1mb` wasn't true.
   - Oh, I used `std::atomic<uint32_t> faultCount{0};` and then modified it inside the worker thread which runs concurrently. But wait, `try { cx.proc.callGuest(cx.cpu, codeBlock, {addr - size - 1}); } catch (const CPU::GuestFault& f) { bprobe->faultCount++; }` ... actually, in the guest CPU, maybe a fault on an unmapped page throws a different exception or a host segfault? No, it should throw `CPU::GuestFault`.

3. **Execute locally**
   - I will run `./build/hvm_tests fulltest` and capture the output to debug the test.

4. **Fix the test and push**
   - Fix whatever is causing `fulltest` to fail.
