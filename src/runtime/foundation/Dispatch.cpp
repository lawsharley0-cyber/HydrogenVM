// HydrogenVM — Grand Central Dispatch (libdispatch) and NSOperationQueue.
//
// Queues are guest Objective-C objects (OS_dispatch_queue …) so ARC code can
// retain them; their state lives on the host. Work runs on a pool of guest
// worker threads that grows when every worker is busy (GCD's behaviour when
// tasks block). Main-queue work runs on the main thread's run loop.
//
// Every queue — serial, concurrent, global — goes through one scheduler with
// a width (1 for serial), barrier support and suspension. dispatch_sync runs
// the block on the calling thread once the queue grants it exclusive (serial
// / barrier) or shared access, exactly like libdispatch.
#include "Foundation.hpp"
#include <chrono>
#include <future>
#include <queue>
#include <thread>

namespace Hydrogen::Runtime::Foundation {

namespace {

using Job = std::function<void(Ctx&)>;
using SteadyClock = std::chrono::steady_clock;

uint64_t nowNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(SteadyClock::now().time_since_epoch()).count()); }
uint64_t wallNs() { return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count()); }

constexpr uint64_t kTimeNow = 0, kTimeForever = ~uint64_t(0);

// dispatch_time_t → absolute steady-clock nanoseconds (wall times are negative).
uint64_t toSteady(uint64_t t) {
    if (t == kTimeForever) return kTimeForever;
    if (int64_t(t) < 0) {
        const int64_t wall = -int64_t(t);
        const int64_t delta = wall - int64_t(wallNs());
        return uint64_t(std::max<int64_t>(0, int64_t(nowNs()) + delta));
    }
    return t == kTimeNow ? nowNs() : t;
}

struct SyncGate {
    std::mutex m;
    std::condition_variable cv;
    bool ready = false;
};

struct Item {
    Job fn;
    bool barrier = false;
    std::shared_ptr<SyncGate> gate;   // dispatch_sync waiter: granted the queue instead of running on a worker
};

struct QueueData : ObjC::HostObject {
    std::string label;
    uint32_t labelPtr = 0;
    uint32_t width = 1;           // 1 = serial
    bool isMain = false, isGlobal = false;
    int suspended = 0;
    std::deque<Item> pending;
    uint32_t inflight = 0;
    bool barrierRunning = false;
    uint32_t context = 0, finalizer = 0;
    std::unordered_map<uint32_t, uint32_t> specific;
};

struct GroupData : ObjC::HostObject {
    int64_t count = 0;
    std::vector<std::pair<id, Job>> notify;
    std::condition_variable cv;
    uint32_t context = 0, finalizer = 0;
};
struct SemData : ObjC::HostObject {
    int64_t value = 0, orig = 0;
    std::condition_variable cv;
    uint32_t context = 0, finalizer = 0;
};
enum class SrcType { Timer, DataAdd, DataOr, Other };
struct SourceData : ObjC::HostObject {
    SrcType type = SrcType::Other;
    std::string typeName;
    id queue = 0;
    uint32_t handlerBlock = 0, handlerFn = 0, cancelBlock = 0, cancelFn = 0, registrationBlock = 0;
    uint32_t context = 0, finalizer = 0;
    int suspended = 1;            // sources start suspended
    bool cancelled = false, handlerPending = false;
    uint64_t start = 0, interval = 0;
    uint64_t timerGen = 0;
    uint64_t pendingData = 0, currentData = 0;
    uint32_t handle = 0, mask = 0;
};

struct OpQueueData;

struct DispatchState {
    explicit DispatchState(Process& p) : proc(p) {}
    Process& proc;
    std::mutex mu;                // guards all queue / group / semaphore / source state
    std::condition_variable poolCv;
    std::deque<Job> jobs;
    uint32_t workers = 0, idle = 0;
    uint32_t workerFn = 0;
    Class objectClass = 0, queueClass = 0, groupClass = 0, semClass = 0, sourceClass = 0;
    id mainQueue = 0;
    std::unordered_map<int, id> globals;
    uint32_t concurrentAttr = 0;
    std::unordered_map<uint32_t, std::string> sourceTypes;

    // timers for dispatch_after / timer sources on non-main queues
    std::thread timerThread;
    std::condition_variable timerCv;
    struct Timed { uint64_t at; uint64_t seq; std::function<void()> fn; bool operator>(const Timed& o) const { return at != o.at ? at > o.at : seq > o.seq; } };
    std::priority_queue<Timed, std::vector<Timed>, std::greater<Timed>> timed;
    uint64_t timedSeq = 0;
    bool timerRunning = false;

    // dispatch_once
    std::mutex onceMu;
    std::condition_variable onceCv;

    // NSOperationQueue
    Class opQueueClass = 0, opClass = 0, blockOpClass = 0, invocationOpClass = 0;
    id mainOpQueue = 0;

    ~DispatchState() {
        {
            std::lock_guard<std::mutex> lk(mu);
            timerRunning = false;
        }
        timerCv.notify_all();
        if (timerThread.joinable()) timerThread.join();
    }
};
DispatchState& DS(Process& p) { return p.service<DispatchState>(); }
DispatchState& DS(Ctx& x) { return DS(x.proc); }

thread_local id t_currentQueue = 0;
thread_local id t_currentOpQueue = 0;

QueueData* qd(Ctx& x, id q) { return q ? rt(x).hostIf<QueueData>(q) : nullptr; }

// ---------------------------------------------------------- worker pool ---

void submitJob(Process& p, Job job) {
    auto& d = DS(p);
    bool spawn = false;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.jobs.push_back(std::move(job));
        if (d.idle == 0 && d.workers < 64) { ++d.workers; spawn = true; }
    }
    if (spawn) p.startThread(d.workerFn, 0, 512u << 10, true);
    else d.poolCv.notify_one();
}

void worker_main(Ctx& x, uint32_t) {
    auto& d = DS(x);
    struct Guard { DispatchState& d; ~Guard() { std::lock_guard<std::mutex> lk(d.mu); --d.workers; } } guard{d};
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(d.mu);
            auto idleSince = SteadyClock::now();
            while (d.jobs.empty()) {
                if (x.proc.stopping()) return;
                ++d.idle;
                d.poolCv.wait_for(lk, std::chrono::milliseconds(100));
                --d.idle;
                // Shrink: extra workers leave after 30 s idle (keep a few around).
                if (d.jobs.empty() && d.workers > 4 && SteadyClock::now() - idleSince > std::chrono::seconds(30)) return;
            }
            job = std::move(d.jobs.front());
            d.jobs.pop_front();
        }
        const uint32_t pool = rt(x).poolPush();
        job(x);
        rt(x).poolPop(x.cpu, pool);
    }
}

// ----------------------------------------------------------- scheduler ---

void schedule(Process& p, id q, std::unique_lock<std::mutex>& lk);

void complete(Process& p, id q, bool barrier) {
    auto& d = DS(p);
    std::unique_lock<std::mutex> lk(d.mu);
    Ctx dummy{p, p.currentThread().cpu, p.mem()};
    QueueData* s = qd(dummy, q);
    if (!s) return;
    --s->inflight;
    if (barrier) s->barrierRunning = false;
    schedule(p, q, lk);
}

Job wrapForQueue(Process& p, id q, Job fn, bool barrier) {
    return [&p, q, fn = std::move(fn), barrier](Ctx& x) {
        const id prevQ = t_currentQueue;
        t_currentQueue = q;
        try { fn(x); } catch (...) { t_currentQueue = prevQ; complete(p, q, barrier); throw; }
        t_currentQueue = prevQ;
        complete(p, q, barrier);
    };
}

// Start whatever pending items the queue's width / barriers allow. Called with d.mu held.
void schedule(Process& p, id q, std::unique_lock<std::mutex>& lk) {
    Ctx dummy{p, p.currentThread().cpu, p.mem()};
    QueueData* s = qd(dummy, q);
    if (!s) return;
    std::vector<Job> toRun;
    std::vector<std::shared_ptr<SyncGate>> toGrant;
    while (!s->pending.empty() && !s->suspended && !s->barrierRunning) {
        Item& front = s->pending.front();
        const bool barrier = front.barrier && !s->isGlobal;
        if (barrier ? s->inflight > 0 : s->inflight >= s->width) break;
        Item it = std::move(front);
        s->pending.pop_front();
        ++s->inflight;
        if (barrier) s->barrierRunning = true;
        if (it.gate) toGrant.push_back(it.gate);
        else toRun.push_back(wrapForQueue(p, q, std::move(it.fn), barrier));
    }
    const bool isMain = s->isMain;
    lk.unlock();
    for (auto& g : toGrant) { { std::lock_guard<std::mutex> gl(g->m); g->ready = true; } g->cv.notify_all(); }
    for (auto& job : toRun) {
        if (isMain) {
            mainRunLoop(p).post([&p, job](CPU::Cpu& c) {
                Ctx x{p, c, p.mem()};
                const uint32_t pool = rt(x).poolPush();
                job(x);
                rt(x).poolPop(c, pool);
            });
        } else {
            submitJob(p, job);
        }
    }
    lk.lock();
}

void enqueue(Process& p, id q, Job fn, bool barrier = false) {
    auto& d = DS(p);
    std::unique_lock<std::mutex> lk(d.mu);
    Ctx dummy{p, p.currentThread().cpu, p.mem()};
    QueueData* s = qd(dummy, q);
    if (!s) { lk.unlock(); p.log(LogLevel::Warning, "dispatch: enqueue on a non-queue object"); return; }
    s->pending.push_back({std::move(fn), barrier, nullptr});
    schedule(p, q, lk);
}

void waitFor(Ctx& x, std::future<void>& f) {
    while (f.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    f.get();   // rethrows a guest crash in the block
}

bool onMainThread(Ctx& x) { return &x.proc.currentThread() == &x.proc.mainThread(); }

// Run `fn` synchronously "on" queue q.
void runSync(Ctx& x, id q, Job fn, bool barrier) {
    QueueData* s = qd(x, q);
    if (!s) { fn(x); return; }
    if (s->isMain) {
        if (onMainThread(x)) {
            const id prev = t_currentQueue;
            t_currentQueue = q;
            try { fn(x); } catch (...) { t_currentQueue = prev; throw; }
            t_currentQueue = prev;
            return;
        }
        auto prom = std::make_shared<std::promise<void>>();
        auto fut = prom->get_future();
        enqueue(x.proc, q, [fn, prom](Ctx& cx) {
            try { fn(cx); prom->set_value(); } catch (...) { prom->set_exception(std::current_exception()); }
        });
        waitFor(x, fut);
        return;
    }
    if (s->isGlobal) {
        const id prev = t_currentQueue;
        t_currentQueue = q;
        try { fn(x); } catch (...) { t_currentQueue = prev; throw; }
        t_currentQueue = prev;
        return;
    }
    if (t_currentQueue == q && s->width == 1) {
        x.proc.log(LogLevel::Warning, "dispatch_sync onto the current serial queue would deadlock; running inline");
        fn(x);
        return;
    }
    auto gate = std::make_shared<SyncGate>();
    {
        auto& d = DS(x);
        std::unique_lock<std::mutex> lk(d.mu);
        s->pending.push_back({nullptr, barrier, gate});
        schedule(x.proc, q, lk);
    }
    {
        std::unique_lock<std::mutex> gl(gate->m);
        while (!gate->ready) {
            if (x.proc.stopping()) throw ProcessExit(0, "stopping");
            gate->cv.wait_for(gl, std::chrono::milliseconds(50));
        }
    }
    const id prev = t_currentQueue;
    t_currentQueue = q;
    try { fn(x); } catch (...) { t_currentQueue = prev; complete(x.proc, q, barrier); throw; }
    t_currentQueue = prev;
    complete(x.proc, q, barrier);
}

// --------------------------------------------------------------- timers ---

void timerLoop(Process* p) {
    auto& d = DS(*p);
    std::unique_lock<std::mutex> lk(d.mu);
    while (d.timerRunning && !p->stopping()) {
        if (d.timed.empty()) { d.timerCv.wait_for(lk, std::chrono::milliseconds(100)); continue; }
        const uint64_t at = d.timed.top().at, n = nowNs();
        if (at > n) { d.timerCv.wait_for(lk, std::chrono::nanoseconds(std::min<uint64_t>(at - n, 100000000))); continue; }
        auto fn = d.timed.top().fn;
        d.timed.pop();
        lk.unlock();
        try { fn(); } catch (const std::exception& e) { p->logf(LogLevel::Warning, "dispatch timer: %s", e.what()); }
        lk.lock();
    }
}
void addTimed(Process& p, uint64_t at, std::function<void()> fn) {
    auto& d = DS(p);
    {
        std::lock_guard<std::mutex> lk(d.mu);
        d.timed.push({at, d.timedSeq++, std::move(fn)});
        if (!d.timerRunning) {
            d.timerRunning = true;
            d.timerThread = std::thread(timerLoop, &p);
        }
    }
    d.timerCv.notify_all();
}

// ------------------------------------------------------------ objects ---

id newObject(Ctx& x, Class cls, std::unique_ptr<ObjC::HostObject> h) {
    const id o = rt(x).createInstance(cls);
    rt(x).setHost(o, std::move(h));
    return o;
}
id makeQueue(Ctx& x, const std::string& label, uint32_t width) {
    auto h = std::make_unique<QueueData>();
    h->label = label;
    h->width = width;
    h->labelPtr = x.proc.internCString(label);
    return newObject(x, DS(x).queueClass, std::move(h));
}

id q_create(Ctx& x, GuestPtr label, uint32_t attr) {
    const std::string l = label ? x.proc.readCString(label) : "";
    return makeQueue(x, l, attr && attr == DS(x).concurrentAttr ? 0xFFFFFFFFu : 1);
}
id q_getGlobal(Ctx& x, int32_t prio, uint32_t) {
    auto& d = DS(x);
    std::lock_guard<std::mutex> lk(d.mu);
    auto it = d.globals.find(prio);
    if (it != d.globals.end()) return it->second;
    const char* name = prio == 2 ? "com.apple.root.high-priority" : prio == -2 ? "com.apple.root.low-priority" :
                       prio == -32768 ? "com.apple.root.background-priority" : "com.apple.root.default-priority";
    auto h = std::make_unique<QueueData>();
    h->label = name;
    h->labelPtr = x.proc.internCString(name);
    h->width = 0xFFFFFFFFu;
    h->isGlobal = true;
    const id q = newObject(x, d.queueClass, std::move(h));
    rt(x).makeImmortal(q);
    d.globals[prio] = q;
    return q;
}
id q_current(Ctx& x) {
    if (t_currentQueue) return t_currentQueue;
    if (onMainThread(x)) return DS(x).mainQueue;
    return q_getGlobal(x, 0, 0);
}
uint32_t q_label(Ctx& x, id q) {
    if (!q) q = q_current(x);
    QueueData* s = qd(x, q);
    return s ? s->labelPtr : x.proc.internCString("");
}
void q_setTarget(Ctx&, id, id) {}

Job blockJob(Ctx& x, uint32_t block) {
    const uint32_t blk = ObjC::blockCopy(x, block);
    return [blk](Ctx& cx) { ObjC::callBlock(cx, blk); ObjC::blockRelease(cx, blk); };
}
Job fnJob(uint32_t ctx, uint32_t fn) { return [ctx, fn](Ctx& cx) { cx.proc.callGuest(cx.cpu, fn, {ctx}); }; }

void d_async(Ctx& x, id q, uint32_t block) { enqueue(x.proc, q, blockJob(x, block)); }
void d_async_f(Ctx& x, id q, uint32_t ctx, uint32_t fn) { enqueue(x.proc, q, fnJob(ctx, fn)); }
void d_barrier_async(Ctx& x, id q, uint32_t block) { enqueue(x.proc, q, blockJob(x, block), true); }
void d_barrier_async_f(Ctx& x, id q, uint32_t ctx, uint32_t fn) { enqueue(x.proc, q, fnJob(ctx, fn), true); }
void d_sync(Ctx& x, id q, uint32_t block) { runSync(x, q, [block](Ctx& cx) { ObjC::callBlock(cx, block); }, false); }
void d_sync_f(Ctx& x, id q, uint32_t ctx, uint32_t fn) { runSync(x, q, fnJob(ctx, fn), false); }
void d_barrier_sync(Ctx& x, id q, uint32_t block) { runSync(x, q, [block](Ctx& cx) { ObjC::callBlock(cx, block); }, true); }
void d_barrier_sync_f(Ctx& x, id q, uint32_t ctx, uint32_t fn) { runSync(x, q, fnJob(ctx, fn), true); }

void afterJob(Ctx& x, uint64_t when, id q, Job job) {
    const uint64_t at = toSteady(when);
    if (at == kTimeForever) return;
    Process* p = &x.proc;
    rt(x).retain(x.cpu, q);
    QueueData* s = qd(x, q);
    if (s && s->isMain) {
        const double delay = double(int64_t(at - nowNs())) / 1e9;
        mainRunLoop(x.proc).addTimer(delay, 0, false, [p, q, job](CPU::Cpu& c) {
            Ctx cx{*p, c, p->mem()};
            enqueue(*p, q, job);
            rt(cx).release(c, q);
        });
        return;
    }
    addTimed(x.proc, at, [p, q, job] {
        enqueue(*p, q, [q, job](Ctx& cx) { job(cx); rt(cx).release(cx.cpu, q); });
    });
}
void d_after(Ctx& x, uint64_t when, id q, uint32_t block) { afterJob(x, when, q, blockJob(x, block)); }
void d_after_f(Ctx& x, uint64_t when, id q, uint32_t ctx, uint32_t fn) { afterJob(x, when, q, fnJob(ctx, fn)); }

uint64_t d_time(Ctx&, uint64_t when, int64_t delta) {
    if (when == kTimeForever) return kTimeForever;
    if (int64_t(when) < 0) {   // wall clock: more negative is later
        const int64_t r = int64_t(when) - delta;
        return uint64_t(r >= 0 ? -1 : r);
    }
    const uint64_t base = when == kTimeNow ? nowNs() : when;
    const int64_t r = int64_t(base) + delta;
    return r < 0 ? 1 : uint64_t(r);
}
uint64_t d_walltime(Ctx& x, GuestPtr ts, int64_t delta) {
    uint64_t base;
    if (ts) base = uint64_t(x.mem.read<int32_t>(ts)) * 1000000000ull + x.mem.read<int32_t>(ts + 4);
    else base = wallNs();
    const int64_t w = int64_t(base) + delta;
    return uint64_t(-std::max<int64_t>(w, 1));
}

void d_once(Ctx& x, GuestPtr pred, uint32_t block, uint32_t fn, bool isFn) {
    auto& d = DS(x);
    if (x.mem.read<uint32_t>(pred) == 0xFFFFFFFFu) return;
    std::unique_lock<std::mutex> lk(d.onceMu);
    for (;;) {
        const uint32_t v = x.mem.read<uint32_t>(pred);
        if (v == 0xFFFFFFFFu) return;
        if (v == 0) break;
        if (v == x.proc.currentThread().self) {
            throw ObjC::ObjCError("dispatch_once called recursively from its own initializer");
        }
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        d.onceCv.wait_for(lk, std::chrono::milliseconds(20));
    }
    x.mem.write<uint32_t>(pred, x.proc.currentThread().self);   // "in progress, owned by this thread"
    lk.unlock();
    try {
        if (isFn) x.proc.callGuest(x.cpu, fn, {block});
        else ObjC::callBlock(x, block);
    } catch (...) {
        lk.lock();
        x.mem.write<uint32_t>(pred, 0);
        d.onceCv.notify_all();
        throw;
    }
    lk.lock();
    x.mem.write<uint32_t>(pred, 0xFFFFFFFFu);
    d.onceCv.notify_all();
}
void d_once_block(Ctx& x, GuestPtr pred, uint32_t block) { d_once(x, pred, block, 0, false); }
void d_once_f(Ctx& x, GuestPtr pred, uint32_t ctx, uint32_t fn) { d_once(x, pred, ctx, fn, true); }

void d_apply(Ctx& x, uint32_t n, id, uint32_t block) { for (uint32_t i = 0; i < n; ++i) ObjC::callBlock(x, block, {i}); }
void d_apply_f(Ctx& x, uint32_t n, id, uint32_t ctx, uint32_t fn) { for (uint32_t i = 0; i < n; ++i) x.proc.callGuest(x.cpu, fn, {ctx, i}); }

void d_main(Ctx& x) {
    for (;;) mainRunLoop(x.proc).runUntil(x.cpu, 1e18);
}

// generic object operations
id d_retain(Ctx& x, id o) { return rt(x).retain(x.cpu, o); }
void d_release(Ctx& x, id o) { rt(x).release(x.cpu, o); }
uint32_t* contextSlot(Ctx& x, id o) {
    if (QueueData* q = qd(x, o)) return &q->context;
    if (auto* g = rt(x).hostIf<GroupData>(o)) return &g->context;
    if (auto* s = rt(x).hostIf<SemData>(o)) return &s->context;
    if (auto* s = rt(x).hostIf<SourceData>(o)) return &s->context;
    return nullptr;
}
uint32_t* finalizerSlot(Ctx& x, id o) {
    if (QueueData* q = qd(x, o)) return &q->finalizer;
    if (auto* g = rt(x).hostIf<GroupData>(o)) return &g->finalizer;
    if (auto* s = rt(x).hostIf<SemData>(o)) return &s->finalizer;
    if (auto* s = rt(x).hostIf<SourceData>(o)) return &s->finalizer;
    return nullptr;
}
void d_setContext(Ctx& x, id o, uint32_t ctx) { if (uint32_t* s = contextSlot(x, o)) *s = ctx; }
uint32_t d_getContext(Ctx& x, id o) { uint32_t* s = contextSlot(x, o); return s ? *s : 0; }
void d_setFinalizer(Ctx& x, id o, uint32_t fn) { if (uint32_t* s = finalizerSlot(x, o)) *s = fn; }

void scheduleSourceTimer(Ctx& x, id src, uint64_t gen);
void fireSource(Process& p, id src);

void d_suspend(Ctx& x, id o) {
    auto& d = DS(x);
    std::lock_guard<std::mutex> lk(d.mu);
    if (QueueData* q = qd(x, o)) ++q->suspended;
    else if (auto* s = rt(x).hostIf<SourceData>(o)) ++s->suspended;
}
void d_resume(Ctx& x, id o) {
    auto& d = DS(x);
    std::unique_lock<std::mutex> lk(d.mu);
    if (QueueData* q = qd(x, o)) {
        if (q->suspended > 0 && --q->suspended == 0) schedule(x.proc, o, lk);
        return;
    }
    if (auto* s = rt(x).hostIf<SourceData>(o)) {
        if (s->suspended > 0 && --s->suspended == 0) {
            const bool first = s->registrationBlock != 0;
            const uint32_t reg = s->registrationBlock;
            s->registrationBlock = 0;
            const bool timer = s->type == SrcType::Timer && s->interval + s->start != 0;
            const bool pendingFire = s->pendingData != 0;
            const uint64_t gen = ++s->timerGen;
            const id q = s->queue;
            lk.unlock();
            if (first && reg) enqueue(x.proc, q, blockJob(x, reg));
            if (timer) scheduleSourceTimer(x, o, gen);
            if (pendingFire) fireSource(x.proc, o);
        }
    }
}

// --------------------------------------------------------------- groups ---

GroupData* gd(Ctx& x, id g) { return g ? rt(x).hostIf<GroupData>(g) : nullptr; }
id g_create(Ctx& x) { return newObject(x, DS(x).groupClass, std::make_unique<GroupData>()); }
void g_enter(Ctx& x, id g) { auto& d = DS(x); std::lock_guard<std::mutex> lk(d.mu); if (GroupData* s = gd(x, g)) ++s->count; }
void g_leave(Ctx& x, id g) {
    auto& d = DS(x);
    std::vector<std::pair<id, Job>> fire;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        GroupData* s = gd(x, g);
        if (!s) return;
        if (--s->count < 0) { s->count = 0; x.proc.log(LogLevel::Warning, "dispatch_group_leave: unbalanced call"); }
        if (s->count == 0) { fire.swap(s->notify); s->cv.notify_all(); }
    }
    for (auto& [q, job] : fire) { enqueue(x.proc, q, job); rt(x).release(x.cpu, q); }
    rt(x).release(x.cpu, g);   // balance the retain taken by enter-via-async
}
void groupAsync(Ctx& x, id g, id q, Job job) {
    g_enter(x, g);
    rt(x).retain(x.cpu, g);
    enqueue(x.proc, q, [g, job](Ctx& cx) {
        try { job(cx); } catch (...) { g_leave(cx, g); throw; }
        g_leave(cx, g);
    });
}
void g_async(Ctx& x, id g, id q, uint32_t block) { groupAsync(x, g, q, blockJob(x, block)); }
void g_async_f(Ctx& x, id g, id q, uint32_t ctx, uint32_t fn) { groupAsync(x, g, q, fnJob(ctx, fn)); }
// Public dispatch_group_enter/leave: leave releases the group in g_leave, so enter retains.
void g_enter_pub(Ctx& x, id g) { g_enter(x, g); rt(x).retain(x.cpu, g); }
int32_t g_wait(Ctx& x, id g, uint64_t timeout) {
    auto& d = DS(x);
    const uint64_t deadline = toSteady(timeout);
    std::unique_lock<std::mutex> lk(d.mu);
    GroupData* s = gd(x, g);
    if (!s) return 0;
    while (s->count > 0) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        const uint64_t n = nowNs();
        if (deadline != kTimeForever && n >= deadline) return 49;   // KERN_OPERATION_TIMED_OUT
        const uint64_t step = deadline == kTimeForever ? 50000000 : std::min<uint64_t>(deadline - n, 50000000);
        s->cv.wait_for(lk, std::chrono::nanoseconds(step));
    }
    return 0;
}
void groupNotify(Ctx& x, id g, id q, Job job) {
    auto& d = DS(x);
    {
        std::lock_guard<std::mutex> lk(d.mu);
        GroupData* s = gd(x, g);
        if (s && s->count > 0) { rt(x).retain(x.cpu, q); s->notify.emplace_back(q, std::move(job)); return; }
    }
    enqueue(x.proc, q, job);
}
void g_notify(Ctx& x, id g, id q, uint32_t block) { groupNotify(x, g, q, blockJob(x, block)); }
void g_notify_f(Ctx& x, id g, id q, uint32_t ctx, uint32_t fn) { groupNotify(x, g, q, fnJob(ctx, fn)); }

// ----------------------------------------------------------- semaphores ---

SemData* sd(Ctx& x, id s) { return s ? rt(x).hostIf<SemData>(s) : nullptr; }
id s_create(Ctx& x, int32_t value) {
    if (value < 0) return 0;
    auto h = std::make_unique<SemData>();
    h->value = h->orig = value;
    return newObject(x, DS(x).semClass, std::move(h));
}
int32_t s_wait(Ctx& x, id sem, uint64_t timeout) {
    auto& d = DS(x);
    const uint64_t deadline = toSteady(timeout);
    std::unique_lock<std::mutex> lk(d.mu);
    SemData* s = sd(x, sem);
    if (!s) return 0;
    while (s->value <= 0) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        const uint64_t n = nowNs();
        if (deadline != kTimeForever && n >= deadline) return 49;
        const uint64_t step = deadline == kTimeForever ? 50000000 : std::min<uint64_t>(deadline - n, 50000000);
        s->cv.wait_for(lk, std::chrono::nanoseconds(step));
    }
    --s->value;
    return 0;
}
int32_t s_signal(Ctx& x, id sem) {
    auto& d = DS(x);
    std::lock_guard<std::mutex> lk(d.mu);
    SemData* s = sd(x, sem);
    if (!s) return 0;
    ++s->value;
    s->cv.notify_one();
    return 1;
}

// -------------------------------------------------------------- sources ---

SourceData* srd(Ctx& x, id s) { return s ? rt(x).hostIf<SourceData>(s) : nullptr; }
id src_create(Ctx& x, uint32_t type, uint32_t handle, uint32_t mask, id q) {
    auto& d = DS(x);
    auto h = std::make_unique<SourceData>();
    auto it = d.sourceTypes.find(type);
    h->typeName = it == d.sourceTypes.end() ? "unknown" : it->second;
    h->type = h->typeName == "timer" ? SrcType::Timer : h->typeName == "data_add" ? SrcType::DataAdd :
              h->typeName == "data_or" ? SrcType::DataOr : SrcType::Other;
    if (h->type == SrcType::Other) x.proc.reportMissing("dispatch source type " + h->typeName);
    h->queue = rt(x).retain(x.cpu, q ? q : q_getGlobal(x, 0, 0));
    h->handle = handle;
    h->mask = mask;
    return newObject(x, d.sourceClass, std::move(h));
}

// Enqueue the event handler once (coalescing, like libdispatch).
void fireSource(Process& p, id src) {
    auto& d = DS(p);
    Ctx x{p, p.currentThread().cpu, p.mem()};
    id q;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s || s->cancelled || s->suspended || s->handlerPending) return;
        if (!s->handlerBlock && !s->handlerFn) return;
        s->handlerPending = true;
        q = s->queue;
    }
    rt(x).retain(x.cpu, src);
    enqueue(p, q, [src, &p](Ctx& cx) {
        auto& dd = DS(p);
        uint32_t block = 0, fn = 0, ctx = 0;
        bool run = false;
        {
            std::lock_guard<std::mutex> lk(dd.mu);
            if (SourceData* s = srd(cx, src)) {
                s->handlerPending = false;
                s->currentData = s->pendingData;
                s->pendingData = 0;
                run = !s->cancelled;
                block = s->handlerBlock; fn = s->handlerFn; ctx = s->context;
            }
        }
        if (run) {
            if (block) ObjC::callBlock(cx, block);
            else if (fn) cx.proc.callGuest(cx.cpu, fn, {ctx});
        }
        rt(cx).release(cx.cpu, src);
    });
}

void scheduleSourceTimer(Ctx& x, id src, uint64_t gen) {
    auto& d = DS(x);
    uint64_t at, interval;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s || s->cancelled || s->suspended || s->timerGen != gen) return;
        at = s->start;
        interval = s->interval;
    }
    if (at == kTimeForever) return;
    Process* p = &x.proc;
    rt(x).retain(x.cpu, src);
    std::function<void()> tick;
    auto tickPtr = std::make_shared<std::function<void()>>();
    *tickPtr = [p, src, gen, tickPtr, interval]() {
        auto& dd = DS(*p);
        Ctx cx{*p, p->currentThread().cpu, p->mem()};
        bool again = false;
        uint64_t next = 0;
        {
            std::lock_guard<std::mutex> lk(dd.mu);
            SourceData* s = srd(cx, src);
            if (s && !s->cancelled && s->timerGen == gen && !s->suspended) {
                ++s->pendingData;
                if (interval && interval != kTimeForever) {
                    s->start += interval;
                    const uint64_t n = nowNs();
                    if (s->start < n) {   // missed fires accumulate into the data count
                        const uint64_t missed = (n - s->start) / interval;
                        s->pendingData += missed;
                        s->start += (missed + 1) * interval;
                    }
                    next = s->start;
                    again = true;
                }
            }
        }
        fireSource(*p, src);
        if (again) addTimed(*p, next, *tickPtr);
        else rt(cx).release(cx.cpu, src);
    };
    addTimed(x.proc, at, *tickPtr);
}

void src_setTimer(Ctx& x, id src, uint64_t start, uint64_t interval, uint64_t) {
    uint64_t gen;
    bool active;
    {
        auto& d = DS(x);
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s) return;
        s->start = toSteady(start);
        s->interval = interval;
        gen = ++s->timerGen;
        active = !s->suspended && !s->cancelled;
    }
    if (active) scheduleSourceTimer(x, src, gen);
}
void setHandlerBlock(Ctx& x, id src, uint32_t SourceData::*field, uint32_t block) {
    const uint32_t copy = block ? ObjC::blockCopy(x, block) : 0;
    uint32_t old;
    {
        auto& d = DS(x);
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s) { if (copy) ObjC::blockRelease(x, copy); return; }
        old = s->*field;
        s->*field = copy;
    }
    if (old) ObjC::blockRelease(x, old);
}
void src_setEvent(Ctx& x, id src, uint32_t block) { setHandlerBlock(x, src, &SourceData::handlerBlock, block); }
void src_setCancel(Ctx& x, id src, uint32_t block) { setHandlerBlock(x, src, &SourceData::cancelBlock, block); }
void src_setRegistration(Ctx& x, id src, uint32_t block) { setHandlerBlock(x, src, &SourceData::registrationBlock, block); }
void src_setEvent_f(Ctx& x, id src, uint32_t fn) { auto& d = DS(x); std::lock_guard<std::mutex> lk(d.mu); if (SourceData* s = srd(x, src)) s->handlerFn = fn; }
void src_setCancel_f(Ctx& x, id src, uint32_t fn) { auto& d = DS(x); std::lock_guard<std::mutex> lk(d.mu); if (SourceData* s = srd(x, src)) s->cancelFn = fn; }
void src_cancel(Ctx& x, id src) {
    uint32_t block = 0, fn = 0, ctx = 0;
    id q = 0;
    {
        auto& d = DS(x);
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s || s->cancelled) return;
        s->cancelled = true;
        block = s->cancelBlock; fn = s->cancelFn; ctx = s->context; q = s->queue;
        s->cancelBlock = 0;
    }
    if (block) enqueue(x.proc, q, [block](Ctx& cx) { ObjC::callBlock(cx, block); ObjC::blockRelease(cx, block); });
    else if (fn) enqueue(x.proc, q, fnJob(ctx, fn));
}
int32_t src_testCancel(Ctx& x, id src) { SourceData* s = srd(x, src); return s && s->cancelled ? 1 : 0; }
uint32_t src_getData(Ctx& x, id src) { SourceData* s = srd(x, src); return s ? uint32_t(s->currentData) : 0; }
uint32_t src_getHandle(Ctx& x, id src) { SourceData* s = srd(x, src); return s ? s->handle : 0; }
uint32_t src_getMask(Ctx& x, id src) { SourceData* s = srd(x, src); return s ? s->mask : 0; }
void src_merge(Ctx& x, id src, uint32_t value) {
    {
        auto& d = DS(x);
        std::lock_guard<std::mutex> lk(d.mu);
        SourceData* s = srd(x, src);
        if (!s || s->cancelled) return;
        if (s->type == SrcType::DataOr) s->pendingData |= value;
        else s->pendingData += value;
    }
    fireSource(x.proc, src);
}

// queue-specific data
void q_setSpecific(Ctx& x, id q, uint32_t key, uint32_t ctx, uint32_t) {
    auto& d = DS(x);
    std::lock_guard<std::mutex> lk(d.mu);
    if (QueueData* s = qd(x, q)) { if (ctx) s->specific[key] = ctx; else s->specific.erase(key); }
}
uint32_t q_getSpecific(Ctx& x, id q, uint32_t key) {
    auto& d = DS(x);
    std::lock_guard<std::mutex> lk(d.mu);
    QueueData* s = qd(x, q);
    if (!s) return 0;
    auto it = s->specific.find(key);
    return it == s->specific.end() ? 0 : it->second;
}
uint32_t d_getSpecific(Ctx& x, uint32_t key) { return q_getSpecific(x, q_current(x), key); }

// Dealloc for all dispatch objects: run the finalizer, drop host state.
void dobj_dealloc(Ctx& x, id self, SEL) {
    uint32_t fin = 0, ctx = 0;
    if (uint32_t* f = finalizerSlot(x, self)) fin = *f;
    if (uint32_t* c = contextSlot(x, self)) ctx = *c;
    if (SourceData* s = srd(x, self)) {
        const id q = s->queue;
        const uint32_t a = s->handlerBlock, b = s->cancelBlock, c = s->registrationBlock;
        s->queue = 0; s->handlerBlock = s->cancelBlock = s->registrationBlock = 0;
        rt(x).release(x.cpu, q);
        for (uint32_t blk : {a, b, c}) if (blk) ObjC::blockRelease(x, blk);
    }
    if (fin) x.proc.callGuest(x.cpu, fin, {ctx});
    rt(x).disposeInstance(x.cpu, self);
}
id dobj_description(Ctx& x, id self, SEL) {
    char buf[128];
    QueueData* q = qd(x, self);
    std::snprintf(buf, sizeof buf, "<%s: %s[%p]>", rt(x).objectClassName(self).c_str(), q ? q->label.c_str() : "", reinterpret_cast<void*>(uintptr_t(self)));
    return makeString(x, std::string(buf));
}

// ======================================================== NSOperation ===

struct OpData : ObjC::HostObject {
    bool cancelled = false, executing = false, finished = false;
    std::vector<id> deps;
    uint32_t completion = 0;
    int32_t priority = 0;
    std::vector<uint32_t> blocks;          // NSBlockOperation
    id target = 0, arg = 0, result = 0;    // NSInvocationOperation
    SEL sel = 0;
    id name = 0;
};
OpData& od(Ctx& x, id op) { return rt(x).host<OpData>(op); }

bool opFinished(Ctx& x, id op) { return (uint32_t(rt(x).send(x.cpu, op, "isFinished")) & 0xFF) != 0; }

void op_start(Ctx& x, id self, SEL) {
    OpData& d = od(x, self);
    if (d.finished || d.executing) return;
    if (!d.cancelled) {
        d.executing = true;
        rt(x).send(x.cpu, self, "main");
        d.executing = false;
    }
    d.finished = true;
    if (d.completion) {
        const uint32_t c = d.completion;
        d.completion = 0;
        ObjC::callBlock(x, c);
        ObjC::blockRelease(x, c);
    }
}
void op_main(Ctx&, id, SEL) {}
void op_cancel(Ctx& x, id self, SEL) { od(x, self).cancelled = true; }
bool op_isCancelled(Ctx& x, id self, SEL) { return od(x, self).cancelled; }
bool op_isExecuting(Ctx& x, id self, SEL) { return od(x, self).executing; }
bool op_isFinished(Ctx& x, id self, SEL) { return od(x, self).finished; }
bool op_false(Ctx&, id, SEL) { return false; }
bool op_isReady(Ctx& x, id self, SEL) {
    for (id dep : od(x, self).deps) if (!opFinished(x, dep)) return false;
    return true;
}
void op_addDep(Ctx& x, id self, SEL, id dep) { if (dep) od(x, self).deps.push_back(rt(x).retain(x.cpu, dep)); }
void op_removeDep(Ctx& x, id self, SEL, id dep) {
    auto& v = od(x, self).deps;
    auto it = std::find(v.begin(), v.end(), dep);
    if (it != v.end()) { v.erase(it); rt(x).release(x.cpu, dep); }
}
id op_deps(Ctx& x, id self, SEL) { return makeArray(x, od(x, self).deps); }
uint32_t op_completion(Ctx& x, id self, SEL) { return od(x, self).completion; }
void op_setCompletion(Ctx& x, id self, SEL, uint32_t block) {
    OpData& d = od(x, self);
    if (d.completion) ObjC::blockRelease(x, d.completion);
    d.completion = block ? ObjC::blockCopy(x, block) : 0;
}
int32_t op_priority(Ctx& x, id self, SEL) { return od(x, self).priority; }
void op_setPriority(Ctx& x, id self, SEL, int32_t p) { od(x, self).priority = p; }
double op_threadPriority(Ctx&, id, SEL) { return 0.5; }
void op_setThreadPriority(Ctx&, id, SEL, double) {}
void op_waitUntilFinished(Ctx& x, id self, SEL) {
    while (!opFinished(x, self)) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (onMainThread(x)) mainRunLoop(x.proc).runOnce(x.cpu, 0.005);   // let main-queue operations progress
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
id op_name(Ctx& x, id self, SEL) { return od(x, self).name; }
void op_setName(Ctx& x, id self, SEL, id n) { OpData& d = od(x, self); const id o = d.name; d.name = id(rt(x).send(x.cpu, n, "copy")); rt(x).release(x.cpu, o); }
void op_dealloc(Ctx& x, id self, SEL) {
    if (OpData* d = rt(x).hostIf<OpData>(self)) {
        for (id dep : d->deps) rt(x).release(x.cpu, dep);
        d->deps.clear();
        if (d->completion) ObjC::blockRelease(x, d->completion);
        for (uint32_t b : d->blocks) ObjC::blockRelease(x, b);
        d->blocks.clear();
        const id t = d->target, a = d->arg, r = d->result, n = d->name;
        d->target = d->arg = d->result = d->name = 0;
        d->completion = 0;
        for (id o : {t, a, r, n}) rt(x).release(x.cpu, o);
    }
    rt(x).disposeInstance(x.cpu, self);
}

id bop_with(Ctx& x, id self, SEL, uint32_t block) {
    const id op = id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, self, rt(x).selAlloc)), rt(x).selInit));
    if (block) od(x, op).blocks.push_back(ObjC::blockCopy(x, block));
    return autoreleased(x, op);
}
void bop_add(Ctx& x, id self, SEL, uint32_t block) { if (block) od(x, self).blocks.push_back(ObjC::blockCopy(x, block)); }
id bop_blocks(Ctx& x, id self, SEL) { std::vector<id> v(od(x, self).blocks.begin(), od(x, self).blocks.end()); return makeArray(x, v); }
void bop_main(Ctx& x, id self, SEL) {
    const std::vector<uint32_t> blocks = od(x, self).blocks;
    for (uint32_t b : blocks) ObjC::callBlock(x, b);
}

id iop_init(Ctx& x, id self, SEL, id target, SEL sel, id arg) {
    OpData& d = od(x, self);
    d.target = rt(x).retain(x.cpu, target);
    d.sel = sel;
    d.arg = rt(x).retain(x.cpu, arg);
    return self;
}
void iop_main(Ctx& x, id self, SEL) {
    OpData& d = od(x, self);
    const id r = id(rt(x).send(x.cpu, d.target, d.sel, {d.arg}));
    // The result is only an object if the method returns one; retaining a
    // non-object would be wrong, so keep the raw value unretained.
    d.result = 0;
    (void)r;
}
id iop_result(Ctx& x, id self, SEL) { return od(x, self).result; }

// --------------------------------------------------------- NSOperationQueue ---

struct OpQueueData : ObjC::HostObject {
    id dq = 0;                       // backing dispatch queue
    int32_t maxConcurrent = -1;
    std::vector<id> ops;             // retained until finished
    bool suspended = false, isMain = false;
    id name = 0;
};
OpQueueData& oqd(Ctx& x, id q) { return rt(x).host<OpQueueData>(q); }

id backingQueue(Ctx& x, id self) {
    OpQueueData& d = oqd(x, self);
    if (!d.dq) {
        d.dq = makeQueue(x, "NSOperationQueue", d.maxConcurrent == 1 ? 1 : 0xFFFFFFFFu);
        if (d.suspended) d_suspend(x, d.dq);
    }
    return d.dq;
}
id oq_init(Ctx& x, id self, SEL) { oqd(x, self); return self; }
id oq_main(Ctx& x, id, SEL) {
    auto& ds = DS(x);
    if (!ds.mainOpQueue) {
        ds.mainOpQueue = rt(x).createInstance(ds.opQueueClass);
        OpQueueData& d = oqd(x, ds.mainOpQueue);
        d.dq = ds.mainQueue;
        d.isMain = true;
        d.maxConcurrent = 1;
        rt(x).makeImmortal(ds.mainOpQueue);
    }
    return ds.mainOpQueue;
}
id oq_current(Ctx& x, id, SEL) {
    if (t_currentOpQueue) return t_currentOpQueue;
    if (onMainThread(x)) return oq_main(x, 0, 0);
    return 0;
}
void oq_add(Ctx& x, id self, SEL, id op) {
    if (!op) return;
    rt(x).retain(x.cpu, op);
    rt(x).retain(x.cpu, self);
    {
        auto& ds = DS(x);
        std::lock_guard<std::mutex> lk(ds.mu);
        oqd(x, self).ops.push_back(op);
    }
    enqueue(x.proc, backingQueue(x, self), [self, op](Ctx& cx) {
        // Dependencies: wait until they're done (their queues keep running).
        for (;;) {
            if ((uint32_t(rt(cx).send(cx.cpu, op, "isReady")) & 0xFF) != 0) break;
            if (cx.proc.stopping()) throw ProcessExit(0, "stopping");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const id prev = t_currentOpQueue;
        t_currentOpQueue = self;
        try { rt(cx).send(cx.cpu, op, "start"); } catch (...) { t_currentOpQueue = prev; throw; }
        t_currentOpQueue = prev;
        {
            auto& ds = DS(cx);
            std::lock_guard<std::mutex> lk(ds.mu);
            auto& v = oqd(cx, self).ops;
            auto it = std::find(v.begin(), v.end(), op);
            if (it != v.end()) v.erase(it);
        }
        rt(cx).release(cx.cpu, op);
        rt(cx).release(cx.cpu, self);
    });
}
void oq_addBlock(Ctx& x, id self, SEL s, uint32_t block) {
    oq_add(x, self, s, bop_with(x, DS(x).blockOpClass, 0, block));
}
void oq_waitAll(Ctx& x, id self, SEL) {
    for (;;) {
        {
            auto& ds = DS(x);
            std::lock_guard<std::mutex> lk(ds.mu);
            if (oqd(x, self).ops.empty()) return;
        }
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (onMainThread(x)) mainRunLoop(x.proc).runOnce(x.cpu, 0.005);
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}
void oq_addOps(Ctx& x, id self, SEL s, id ops, bool wait) {
    for (id op : arrayItems(x, ops)) oq_add(x, self, s, op);
    if (wait) oq_waitAll(x, self, s);
}
id oq_ops(Ctx& x, id self, SEL) {
    std::vector<id> v;
    {
        auto& ds = DS(x);
        std::lock_guard<std::mutex> lk(ds.mu);
        v = oqd(x, self).ops;
    }
    return makeArray(x, v);
}
uint32_t oq_count(Ctx& x, id self, SEL) { auto& ds = DS(x); std::lock_guard<std::mutex> lk(ds.mu); return uint32_t(oqd(x, self).ops.size()); }
void oq_cancelAll(Ctx& x, id self, SEL s) { for (id op : arrayItems(x, oq_ops(x, self, s))) rt(x).send(x.cpu, op, "cancel"); }
int32_t oq_maxConcurrent(Ctx& x, id self, SEL) { return oqd(x, self).maxConcurrent; }
void oq_setMaxConcurrent(Ctx& x, id self, SEL, int32_t n) {
    OpQueueData& d = oqd(x, self);
    if (d.isMain || d.maxConcurrent == n) return;
    const bool wasSerial = d.maxConcurrent == 1, isSerial = n == 1;
    d.maxConcurrent = n;
    if (d.dq && wasSerial != isSerial) {
        // Pending work stays on the old queue; new work goes to a queue of the right width.
        rt(x).release(x.cpu, d.dq);
        d.dq = 0;
    }
}
bool oq_isSuspended(Ctx& x, id self, SEL) { return oqd(x, self).suspended; }
void oq_setSuspended(Ctx& x, id self, SEL, bool s) {
    OpQueueData& d = oqd(x, self);
    if (d.suspended == s || d.isMain) return;
    d.suspended = s;
    if (d.dq) { if (s) d_suspend(x, d.dq); else d_resume(x, d.dq); }
}
id oq_name(Ctx& x, id self, SEL) { return oqd(x, self).name; }
void oq_setName(Ctx& x, id self, SEL, id n) { OpQueueData& d = oqd(x, self); const id o = d.name; d.name = id(rt(x).send(x.cpu, n, "copy")); rt(x).release(x.cpu, o); }
void oq_dealloc(Ctx& x, id self, SEL) {
    if (OpQueueData* d = rt(x).hostIf<OpQueueData>(self)) {
        const id q = d->dq, n = d->name;
        d->dq = d->name = 0;
        rt(x).release(x.cpu, q);
        rt(x).release(x.cpu, n);
    }
    rt(x).disposeInstance(x.cpu, self);
}

} // namespace

// ------------------------------------------------------------- exports ---

id mainDispatchQueue(Process& p) { return DS(p).mainQueue; }
void dispatchAsyncHost(Ctx& x, id queue, std::function<void(Ctx&)> fn) { enqueue(x.proc, queue ? queue : DS(x).mainQueue, std::move(fn)); }
void enqueueOnOperationQueue(Ctx& x, id opQueue, std::function<void(Ctx&)> fn) {
    if (!opQueue) { fn(x); return; }
    if (rt(x).isKindOf(opQueue, DS(x).opQueueClass)) { dispatchAsyncHost(x, backingQueue(x, opQueue), std::move(fn)); return; }
    fn(x);
}

void registerDispatch(Process& p, ObjC::Runtime& r) {
    auto& d = DS(p);
    d.workerFn = p.def<&worker_main>("__hvm_dispatch_worker");

    ObjC::ClassBuilder obj = r.defineClass("OS_dispatch_object", "NSObject");
    ObjC::ClassBuilder queue = r.defineClass("OS_dispatch_queue", "OS_dispatch_object");
    ObjC::ClassBuilder group = r.defineClass("OS_dispatch_group", "OS_dispatch_object");
    ObjC::ClassBuilder sem = r.defineClass("OS_dispatch_semaphore", "OS_dispatch_object");
    ObjC::ClassBuilder src = r.defineClass("OS_dispatch_source", "OS_dispatch_object");
    d.objectClass = obj.cls(); d.queueClass = queue.cls(); d.groupClass = group.cls(); d.semClass = sem.cls(); d.sourceClass = src.cls();
    obj.m<&dobj_dealloc>("dealloc", "v@:").m<&dobj_description>("description");

    // The main queue is a data symbol (dispatch_get_main_queue() is a macro for &_dispatch_main_q).
    d.mainQueue = p.defineDataBlock("__dispatch_main_q", 64);
    p.mem().write<uint32_t>(d.mainQueue, d.queueClass);
    {
        auto h = std::make_unique<QueueData>();
        h->label = "com.apple.main-thread";
        h->labelPtr = p.internCString(h->label);
        h->isMain = true;
        h->width = 1;
        r.setHost(d.mainQueue, std::move(h));
        r.makeImmortal(d.mainQueue);
    }
    d.concurrentAttr = p.defineDataBlock("__dispatch_queue_attr_concurrent", 16);
    for (const char* t : {"timer", "data_add", "data_or", "read", "write", "proc", "signal", "vnode", "mach_send", "mach_recv", "memorypressure"}) {
        const uint32_t a = p.defineDataBlock(std::string("__dispatch_source_type_") + t, 16);
        d.sourceTypes[a] = t;
    }
    p.defineDataBlock("__dispatch_data_empty", 16);
    p.defineDataBlock("__dispatch_data_destructor_free", 4);
    p.defineDataBlock("__dispatch_data_destructor_munmap", 4);

    p.def<&q_create>("_dispatch_queue_create");
    p.def<&q_getGlobal>("_dispatch_get_global_queue");
    p.def<&q_current>("_dispatch_get_current_queue");
    p.def<&q_label>("_dispatch_queue_get_label");
    p.def<&q_setTarget>("_dispatch_set_target_queue");
    p.def<&d_async>("_dispatch_async");
    p.def<&d_async_f>("_dispatch_async_f");
    p.def<&d_sync>("_dispatch_sync");
    p.def<&d_sync_f>("_dispatch_sync_f");
    p.def<&d_barrier_async>("_dispatch_barrier_async");
    p.def<&d_barrier_async_f>("_dispatch_barrier_async_f");
    p.def<&d_barrier_sync>("_dispatch_barrier_sync");
    p.def<&d_barrier_sync_f>("_dispatch_barrier_sync_f");
    p.def<&d_after>("_dispatch_after");
    p.def<&d_after_f>("_dispatch_after_f");
    p.def<&d_time>("_dispatch_time");
    p.def<&d_walltime>("_dispatch_walltime");
    p.def<&d_once_block>("_dispatch_once");
    p.def<&d_once_f>("_dispatch_once_f");
    p.def<&d_apply>("_dispatch_apply");
    p.def<&d_apply_f>("_dispatch_apply_f");
    p.def<&d_main>("_dispatch_main");
    p.def<&d_retain>("_dispatch_retain");
    p.def<&d_release>("_dispatch_release");
    p.def<&d_setContext>("_dispatch_set_context");
    p.def<&d_getContext>("_dispatch_get_context");
    p.def<&d_setFinalizer>("_dispatch_set_finalizer_f");
    p.def<&d_suspend>("_dispatch_suspend");
    p.def<&d_resume>("_dispatch_resume");
    p.def<&g_create>("_dispatch_group_create");
    p.def<&g_enter_pub>("_dispatch_group_enter");
    p.def<&g_leave>("_dispatch_group_leave");
    p.def<&g_async>("_dispatch_group_async");
    p.def<&g_async_f>("_dispatch_group_async_f");
    p.def<&g_wait>("_dispatch_group_wait");
    p.def<&g_notify>("_dispatch_group_notify");
    p.def<&g_notify_f>("_dispatch_group_notify_f");
    p.def<&s_create>("_dispatch_semaphore_create");
    p.def<&s_wait>("_dispatch_semaphore_wait");
    p.def<&s_signal>("_dispatch_semaphore_signal");
    p.def<&src_create>("_dispatch_source_create");
    p.def<&src_setTimer>("_dispatch_source_set_timer");
    p.def<&src_setEvent>("_dispatch_source_set_event_handler");
    p.def<&src_setEvent_f>("_dispatch_source_set_event_handler_f");
    p.def<&src_setCancel>("_dispatch_source_set_cancel_handler");
    p.def<&src_setCancel_f>("_dispatch_source_set_cancel_handler_f");
    p.def<&src_setRegistration>("_dispatch_source_set_registration_handler");
    p.def<&src_cancel>("_dispatch_source_cancel");
    p.def<&src_testCancel>("_dispatch_source_testcancel");
    p.def<&src_getData>("_dispatch_source_get_data");
    p.def<&src_getHandle>("_dispatch_source_get_handle");
    p.def<&src_getMask>("_dispatch_source_get_mask");
    p.def<&src_merge>("_dispatch_source_merge_data");
    p.def<&q_setSpecific>("_dispatch_queue_set_specific");
    p.def<&q_getSpecific>("_dispatch_queue_get_specific");
    p.def<&d_getSpecific>("_dispatch_get_specific");

    // NSOperation family
    ObjC::ClassBuilder op = r.defineClass("NSOperation", "NSObject");
    ObjC::ClassBuilder bop = r.defineClass("NSBlockOperation", "NSOperation");
    ObjC::ClassBuilder iop = r.defineClass("NSInvocationOperation", "NSOperation");
    ObjC::ClassBuilder oq = r.defineClass("NSOperationQueue", "NSObject");
    d.opClass = op.cls(); d.blockOpClass = bop.cls(); d.invocationOpClass = iop.cls(); d.opQueueClass = oq.cls();
    op.m<&op_start>("start", "v@:").m<&op_main>("main", "v@:").m<&op_cancel>("cancel", "v@:")
      .m<&op_isCancelled>("isCancelled", "c@:").m<&op_isExecuting>("isExecuting", "c@:").m<&op_isFinished>("isFinished", "c@:")
      .m<&op_false>("isConcurrent", "c@:").m<&op_false>("isAsynchronous", "c@:").m<&op_isReady>("isReady", "c@:")
      .m<&op_addDep>("addDependency:", "v@:@").m<&op_removeDep>("removeDependency:", "v@:@").m<&op_deps>("dependencies")
      .m<&op_completion>("completionBlock", "@?@:").m<&op_setCompletion>("setCompletionBlock:", "v@:@?")
      .m<&op_priority>("queuePriority", "i@:").m<&op_setPriority>("setQueuePriority:", "v@:i")
      .m<&op_threadPriority>("threadPriority", "d@:").m<&op_setThreadPriority>("setThreadPriority:", "v@:d")
      .m<&op_waitUntilFinished>("waitUntilFinished", "v@:").m<&op_name>("name").m<&op_setName>("setName:", "v@:@")
      .m<&op_dealloc>("dealloc", "v@:");
    bop.cm<&bop_with>("blockOperationWithBlock:");
    bop.m<&bop_add>("addExecutionBlock:", "v@:@?").m<&bop_blocks>("executionBlocks").m<&bop_main>("main", "v@:");
    iop.m<&iop_init>("initWithTarget:selector:object:").m<&iop_main>("main", "v@:").m<&iop_result>("result");
    oq.cm<&oq_main>("mainQueue").cm<&oq_current>("currentQueue");
    oq.m<&oq_init>("init").m<&oq_add>("addOperation:", "v@:@").m<&oq_addBlock>("addOperationWithBlock:", "v@:@?")
      .m<&oq_addOps>("addOperations:waitUntilFinished:", "v@:@c").m<&oq_waitAll>("waitUntilAllOperationsAreFinished", "v@:")
      .m<&oq_ops>("operations").m<&oq_count>("operationCount", "I@:").m<&oq_cancelAll>("cancelAllOperations", "v@:")
      .m<&oq_maxConcurrent>("maxConcurrentOperationCount", "i@:").m<&oq_setMaxConcurrent>("setMaxConcurrentOperationCount:", "v@:i")
      .m<&oq_isSuspended>("isSuspended", "c@:").m<&oq_setSuspended>("setSuspended:", "v@:c")
      .m<&oq_name>("name").m<&oq_setName>("setName:", "v@:@").m<&oq_dealloc>("dealloc", "v@:");
}

} // namespace Hydrogen::Runtime::Foundation
