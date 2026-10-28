If we change `Process::~Process` to do a bounded wait (wait_for 50ms in a loop, until a total timeout is reached, e.g., 200 iterations for 2s), and then break the wait and return, `Process` gets destroyed.
If `Process` gets destroyed, `GuestThread` objects live on (because they are `shared_ptr` captured by the host thread lambdas).
BUT the host thread lambda accesses `this->threadMain` and `this->engine_` etc.!
To protect against this, we can wrap the lambda logic:
If `callGuest` blocks indefinitely, the thread is inside `engine_->callGuest`. When `~Process` returns, `engine_` is deleted. The blocked thread will segmentation fault if it tries to access `engine_` memory (which is now freed).
Wait, if `engine_` is a `unique_ptr`, we can change it to a `shared_ptr`!
If `engine_` is a `shared_ptr<CPU::Engine>`, it will NOT be destroyed when `Process` is destroyed, as long as the detached worker thread holds a reference to it!
Let's see `src/runtime/Process.hpp`:
```cpp
    std::unique_ptr<Memory::GuestArena> mem_;
    std::unique_ptr<CPU::Engine> engine_;
```
If we change `engine_` to `shared_ptr<CPU::Engine>`, it still accesses `mem_`. So `mem_` also needs to be `shared_ptr`?
Wait! "The detached callback also needs protection against accessing destroyed Process/thread state."
This implies we shouldn't keep everything alive, but rather *protect* the detached callback from accessing the destroyed state.
If the thread is blocked in an HLE call, it is blocked inside a host function (like a `sleep` or reading a socket).
If `~Process` destroys `Process`, the HLE call might access `x.proc`.
Ah... "with a blocked-worker regression that finishes within a finite deadline"
Maybe the user means: `threadMain` is what blocks?
Wait, if a worker is blocked, how does it unblock?
`requestStop()` sets `c.exit = CPU::Exit::Halt`. The CPU interpreter checks this periodically.
But if it's blocked in a host HLE call, it's sleeping.
If we use `wait_for`, and the timeout expires, the emulator forcefully exits. But does it?
The user says: "the new for (;;) / unconditional activeThread->cv.wait() during Process destruction creates a potentially unbounded shutdown hang if a worker never completes."
So we MUST revert to a bounded wait (e.g. 200 * 10ms) in `~Process()`.
```cpp
    for (int i = 0; i < 200; ++i) {
        std::shared_ptr<GuestThread> activeThread;
        {
            std::lock_guard<std::mutex> lk(threadsMu_);
            for (auto& [k, t] : threads_) {
                if (!t->finished.load()) { activeThread = t; break; }
            }
        }
        if (!activeThread) break;
        std::unique_lock<std::mutex> lk(activeThread->m);
        if (activeThread->cv.wait_for(lk, std::chrono::milliseconds(10), [&] { return activeThread->finished.load(); })) break;
    }
```
If it times out, the worker is left running.
To prevent the worker from crashing when `Process` is destroyed, we can have the lambda capture `weak_ptr`? No, `Process` is not `shared_ptr`.
We can capture the `liveness_` token:
```cpp
    spawnHostThread([this, raw, fn, arg, alive = liveness_, threadRef = std::move(t)] {
        {
            std::lock_guard<std::mutex> lk(alive->mu);
            if (!alive->alive) {
                std::lock_guard<std::mutex> tlk(threadRef->m);
                threadRef->finished = true;
                threadRef->cv.notify_all();
                return;
            }
        }
        try {
            t_thread = raw;
            CPU::Engine::lastFault() = {};
            raw->retval = uint32_t(engine_->callGuest(raw->cpu, fn, {arg}));
        } catch (const CPU::GuestFault& f) {
            std::lock_guard<std::mutex> lk(alive->mu);
            if (alive->alive && !raw->pthreadExited && !exiting_ && !stopping()) finishCrash(raw->cpu, f.exit);
        } catch (const std::exception& e) {
            std::lock_guard<std::mutex> alk(alive->mu);
            if (alive->alive && !exiting_ && !stopping()) {
                std::lock_guard<std::mutex> lk(exitMu_);
                if (!crashed_) {
                    crashed_ = true;
                    crashReport_ = std::string("HydrogenVM error: ") + e.what();
                    captureCrash(raw->cpu, "hle-exception", e.what());
                    log(LogLevel::Error, crashReport_);
                    engine_->requestStop();
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(raw->m);
            raw->finished = true;
        }
        raw->cv.notify_all();
        t_thread = nullptr;
    });
```
Wait, if the thread is BLOCKED inside `engine_->callGuest(raw->cpu, fn, {arg})`!
If `engine_->callGuest` is blocked and `~Process` times out, `Process` is destroyed! `engine_` is destroyed!
BUT `callGuest` is EXECUTING on the host thread. The memory for `engine_` will be unmapped. The host thread will SIGSEGV when it accesses `engine_` fields.
The ONLY way to avoid a SIGSEGV is if the worker thread is terminated, OR if `Process` is kept alive until the worker finishes, OR if the worker is somehow guaranteed to not access `Process`.
"Please revise to a bounded, cancellation-aware teardown with a blocked-worker regression that finishes within a finite deadline."
If `engine_->requestStop()` is called in `~Process`, it cancels `engine_->callGuest()`. That's the "cancellation-aware teardown"!
But if the worker ignores it or is blocked in a custom HLE function that doesn't check `requestStop()`, it hangs.
So if we revert to a bounded wait in `~Process`:
```cpp
    // Wait (bounded) for guest threads to notice and exit.
    for (int i = 0; i < 200; ++i) {
        bool alive = false;
        {
            std::lock_guard<std::mutex> lk(threadsMu_);
            for (auto& [k, t] : threads_) alive |= !t->finished.load();
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
```
Wait, the original code DID exactly this!
```cpp
    // Wait (bounded) for guest threads to notice.
    for (int i = 0; i < 200; ++i) {
        bool alive = false;
        {
            std::lock_guard<std::mutex> lk(threadsMu_);
            for (auto& [k, t] : threads_) alive |= !t->finished.load();
        }
        if (!alive) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
```
If we restore this bounded wait (but maybe using `cv.wait_for` so it's not a busy wait), and the thread times out, `Process` is destroyed.
Then the detached callback accesses destroyed state.
How to protect it?
"The detached callback also needs protection against accessing destroyed Process/thread state."
If `threadMain` is running, it will eventually do:
```cpp
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->finished = true;
    }
    t->cv.notify_all();
    t_thread = nullptr;
```
If `t` is a `shared_ptr`, `t->m` and `t->finished` are perfectly safe to access because `t` is kept alive by the lambda capture!
But `t_thread` is a global thread-local.
Wait, if `threadMain` finishes, it returns to the lambda. The lambda returns, and the host thread terminates.
The original issue was exactly:
"when a timed --run-for shutdown occurs, detached worker threads completing threadMain() do not dereference freed GuestThread mutexes or finished flags."
YES! Because `t` (the `GuestThread`) was owned by `threads_` which is destroyed by `~Process`.
So making `GuestThread` a `shared_ptr` ALREADY protected the detached callback against accessing destroyed *thread* state!
What about destroyed *Process* state?
Well, in `threadMain`, we access `t->retval = uint32_t(engine_->callGuest(t->cpu, fn, {arg}));`
If `engine_->callGuest()` is stuck, and `~Process` times out, `engine_` is destroyed, and it will crash inside `callGuest`. But the PR states: "The detached callback also needs protection against accessing destroyed Process/thread state."
Maybe it means `Process` state *after* `callGuest` completes?
After `callGuest` completes, it goes into `catch` block (which accesses `Process`) or straight to:
```cpp
    {
        std::lock_guard<std::mutex> lk(t->m);
        t->finished = true;
    }
    t->cv.notify_all();
    t_thread = nullptr;
```
Wait! `Process::threadMain` accesses `this` implicitly!
If we change `threadMain` so it doesn't access `this` at the end?
