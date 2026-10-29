import sys

# Pthread.cpp
content = open('src/runtime/libc/Pthread.cpp').read()

old_condwait = """int condWait(Ctx& x, GuestPtr cv, GuestPtr mu, int64_t deadlineNs) {
    checkStop(x);
    t_testcancel(x);
    GuestCond& c = condFor(x, cv);
    GuestMutex& m = mutexFor(x, mu);
    uint64_t gen;
    { std::lock_guard<std::mutex> lk(c.m); gen = c.gen; }
    // Fully release the mutex (it may be recursively held).
    int saved;
    {
        std::lock_guard<std::mutex> lk(m.m);
        if (m.owner != tid(x)) return D_EPERM;
        saved = m.count;
        m.owner = 0; m.count = 0;
        m.cv.notify_one();
    }
    int result = 0;
    {
        std::unique_lock<std::mutex> lk(c.m);
        while (c.gen == gen) {
            if (x.proc.stopping() || cancelPending(x)) { result = -1; break; }
            c.cv.wait_for(lk, 50ms);
            if (x.proc.stopping() || cancelPending(x)) { result = -1; break; }
            if (c.gen != gen) break;
            if (deadlineNs) {
                const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if (now >= deadlineNs) { result = D_ETIMEDOUT; break; }
            }
        }
    }
    lockMutex(x, m, false);
    { std::lock_guard<std::mutex> lk(m.m); m.count = saved; }
    t_testcancel(x); // POSIX requires the mutex to be reacquired before cancellation.
    checkStop(x);
    return result;
}"""

new_condwait = """int condWait(Ctx& x, GuestPtr cv, GuestPtr mu, int64_t deadlineNs) {
    checkStop(x);
    t_testcancel(x);
    GuestCond& c = condFor(x, cv);
    GuestMutex& m = mutexFor(x, mu);
    uint64_t gen;
    { std::lock_guard<std::mutex> lk(c.m); gen = c.gen; }
    // Fully release the mutex (it may be recursively held).
    int saved;
    {
        std::lock_guard<std::mutex> lk(m.m);
        if (m.owner != tid(x)) return D_EPERM;
        saved = m.count;
        m.owner = 0; m.count = 0;
        m.cv.notify_one();
    }
    int result = 0;
    {
        std::unique_lock<std::mutex> lk(c.m);
        while (c.gen == gen) {
            if (x.proc.stopping() || cancelPending(x)) { result = -1; break; }
            c.cv.wait_for(lk, 50ms);
            if (x.proc.stopping() || cancelPending(x)) { result = -1; break; }
            if (c.gen != gen) break;
            if (deadlineNs) {
                const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
                if (now >= deadlineNs) { result = D_ETIMEDOUT; break; }
            }
        }
    }
    struct MutexRestorer {
        GuestMutex& m; int saved; bool locked;
        ~MutexRestorer() { if (locked) { std::lock_guard<std::mutex> lk(m.m); m.count = saved; } }
    } restorer{m, saved, false};
    lockMutex(x, m, false);
    restorer.locked = true;
    checkStop(x);
    t_testcancel(x); // POSIX requires the mutex to be reacquired before cancellation.
    return result;
}"""

content = content.replace(old_condwait, new_condwait)

old_bcast = """void pthreadsBroadcastStop(Process& p) {
    auto& t = p.service<Threads>();
    std::lock_guard<std::mutex> lk(t.mu);
    for (auto& [addr, c] : t.conds) {
        std::lock_guard<std::mutex> clk(c->m);
        ++c->gen;
        c->cv.notify_all();
    }
}"""

new_bcast = """void pthreadsBroadcastStop(Process& p) {
    auto& t = p.service<Threads>();
    std::vector<GuestCond*> conds;
    { std::lock_guard<std::mutex> lk(t.mu); for (auto& [addr, c] : t.conds) conds.push_back(c.get()); }
    for (auto* c : conds) {
        std::lock_guard<std::mutex> clk(c->m);
        ++c->gen;
        c->cv.notify_all();
    }
}"""

content = content.replace(old_bcast, new_bcast)

with open('src/runtime/libc/Pthread.cpp', 'w') as f:
    f.write(content)

print("Pthread.cpp updated")

# TestRunner.cpp
content = open('tests/runner/TestRunner.cpp').read()

old_test = """    // 2f13. Pthread condition variable process stop broadcast
    {
        ProcessConfig pcfg;
        pcfg.homePath = "/tmp/hvm_pthreads_stop";
        Process isolated(pcfg);
        auto& icpu = isolated.mainThread().cpu;

        const uint32_t cv = isolated.heap().allocZeroed(4);
        const uint32_t mu = isolated.heap().allocZeroed(4);
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_init"), {mu, 0});
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_cond_init"), {cv, 0});

        const uint32_t readyFlag = isolated.heap().allocZeroed(4);
        std::atomic<bool> workerFinished{false};
        const uint32_t waiterFn = isolated.registerHostFunction("waiter", [cv, mu, readyFlag, &workerFinished](Runtime::Ctx& x) {
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_mutex_lock"), {mu});
            x.proc.mem().write<uint32_t>(readyFlag, 1);
            try {
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_cond_wait"), {cv, mu});
            } catch (const Runtime::ProcessExit&) {
            } catch (const CPU::GuestFault&) {} // Catch ProcessExit or stopping gracefully
            workerFinished = true;
        });

        isolated.startThread(waiterFn, 0, 16u << 10, false);
        const auto tStart = std::chrono::steady_clock::now();
        while (isolated.mem().read<uint32_t>(readyFlag) == 0 && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        while (isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_trylock"), {mu}) != 0 && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_unlock"), {mu});

        const auto t0 = std::chrono::steady_clock::now();
        isolated.requestStop();

        while (!workerFinished && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) std::this_thread::yield();
        const auto t1 = std::chrono::steady_clock::now();

        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        check(workerFinished.load() && ms < 45.0, "pthread condition variable process stop broadcast wakes up and joins quickly");
    }"""

new_test = """    // 2f13. Pthread condition variable process stop broadcast
    {
        ProcessConfig pcfg;
        pcfg.homePath = "/tmp/hvm_pthreads_stop";
        Process isolated(pcfg);
        auto& icpu = isolated.mainThread().cpu;

        const uint32_t cv = isolated.heap().allocZeroed(4);
        const uint32_t mu = isolated.heap().allocZeroed(4);
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_init"), {mu, 0});
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_cond_init"), {cv, 0});

        std::atomic<int> readyCount{0};
        std::atomic<int> finishedCount{0};
        const uint32_t waiterFn = isolated.registerHostFunction("waiter", [cv, mu, &readyCount, &finishedCount](Runtime::Ctx& x) {
            x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_mutex_lock"), {mu});
            ++readyCount;
            try {
                x.proc.callGuest(x.cpu, *x.proc.lookupSymbol("_pthread_cond_wait"), {cv, mu});
            } catch (const Runtime::ProcessExit&) {
            } catch (const CPU::GuestFault&) {}
            ++finishedCount;
        });

        for (int i = 0; i < 3; ++i) isolated.startThread(waiterFn, 0, 16u << 10, false);

        const auto tStart = std::chrono::steady_clock::now();
        while (readyCount < 3 && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        while (isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_trylock"), {mu}) != 0 && std::chrono::steady_clock::now() - tStart < std::chrono::seconds(2)) std::this_thread::yield();
        isolated.callGuest(icpu, *isolated.lookupSymbol("_pthread_mutex_unlock"), {mu});

        const auto t0 = std::chrono::steady_clock::now();
        isolated.requestStop();

        while (finishedCount < 3 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) std::this_thread::yield();
        const auto t1 = std::chrono::steady_clock::now();

        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        check(finishedCount.load() == 3 && ms < 35.0, "pthread condition variable process stop broadcast wakes up and joins quickly");
    }"""

content = content.replace(old_test, new_test)

with open('tests/runner/TestRunner.cpp', 'w') as f:
    f.write(content)

print("TestRunner.cpp updated")
