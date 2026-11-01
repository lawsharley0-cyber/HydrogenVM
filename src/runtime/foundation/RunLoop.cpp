// HydrogenVM — run loops, timers, dates, delayed/cross-thread performs, NSThread.
#include "Foundation.hpp"
#include <algorithm>
#include <mutex>
#include <chrono>
#include <future>
#include <thread>

namespace Hydrogen::Runtime::Foundation {

double now() {
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count() - 978307200.0;
}

// ------------------------------------------------------------- RunLoop ---

std::shared_ptr<RunLoop::Timer> RunLoop::addTimer(double delay, double interval, bool repeats, std::function<void(CPU::Cpu&)> fire, id timerObj) {
    auto t = std::make_shared<Timer>();
    t->timer = timerObj;
    t->fireAt = now() + std::max(0.0, delay);
    t->interval = interval;
    t->repeats = repeats;
    t->fire = std::move(fire);
    {
        std::lock_guard<std::mutex> lk(mu);
        timers.push_back(t);
    }
    cv.notify_all();
    return t;
}

void RunLoop::addSource(const std::shared_ptr<Source>& s) {
    {
        std::lock_guard<std::mutex> lk(mu);
        if (std::find(sources.begin(), sources.end(), s) == sources.end()) sources.push_back(s);
    }
    cv.notify_all();   // it may already be signalled
}
void RunLoop::removeSource(const std::shared_ptr<Source>& s) {
    std::lock_guard<std::mutex> lk(mu);
    sources.erase(std::remove(sources.begin(), sources.end(), s), sources.end());
}
bool RunLoop::hasSource(const std::shared_ptr<Source>& s) {
    std::lock_guard<std::mutex> lk(mu);
    return std::find(sources.begin(), sources.end(), s) != sources.end();
}

void RunLoop::post(std::function<void(CPU::Cpu&)> fn) {
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    cv.notify_all();
}

bool RunLoop::runOnce(CPU::Cpu& c, double maxWait) {
    if (proc.terminating() || proc.stopping()) return false;
    std::deque<std::function<void(CPU::Cpu&)>> work;
    std::vector<std::shared_ptr<Timer>> due;
    std::vector<std::shared_ptr<Source>> ready;
    std::vector<std::function<void(CPU::Cpu&, double)>> hooks;
    double nextDeadline;
    bool frameDue = false;
    {
        std::unique_lock<std::mutex> lk(mu);
        double t = now();
        nextDeadline = t + maxWait;
        timers.erase(std::remove_if(timers.begin(), timers.end(), [](auto& tm) { return !tm->valid; }), timers.end());
        for (auto& tm : timers) {
            if (tm->fireAt <= t) due.push_back(tm);
            else nextDeadline = std::min(nextDeadline, tm->fireAt);
        }
        // Signalled sources: claiming one clears its flag, so a source shared by two loops performs once.
        const auto claimSources = [&] {
            sources.erase(std::remove_if(sources.begin(), sources.end(), [](auto& s) { return !s->valid.load(); }), sources.end());
            for (auto& s : sources)
                if (s->signalled.exchange(false) && std::find(ready.begin(), ready.end(), s) == ready.end()) ready.push_back(s);
        };
        if (!sources.empty()) claimSources();
        if (!frameHooks.empty()) nextDeadline = std::min(nextDeadline, nextFrameAt);
        if (queue.empty() && due.empty() && ready.empty() && nextDeadline > t) {
            cv.wait_for(lk, std::chrono::duration<double>(std::min(nextDeadline - t, 0.05)));
            if (queue.empty()) {
                t = now();
                for (auto& tm : timers) if (tm->valid && tm->fireAt <= t && std::find(due.begin(), due.end(), tm) == due.end()) due.push_back(tm);
            }
            if (!sources.empty()) claimSources();
        }
        std::stable_sort(ready.begin(), ready.end(), [](auto& a, auto& b) { return a->order < b->order; });
        work.swap(queue);
        t = now();
        if (!frameHooks.empty() && t >= nextFrameAt) {
            frameDue = true;
            hooks = frameHooks;
            // 60 Hz cadence; if we fell behind, don't try to catch up with a burst of frames.
            nextFrameAt = std::max(nextFrameAt + 1.0 / 60.0, t + 0.004);
        }
    }
    bool ran = !work.empty();
    for (auto& fn : work) {
        if (proc.terminating() || proc.stopping()) return ran;
        fn(c);
    }
    for (auto& s : ready) {
        if (proc.terminating() || proc.stopping()) return ran;
        if (!s->valid.load() || !s->perform) continue;
        ran = true;
        s->perform(c);
    }
    for (auto& tm : due) {
        if (proc.terminating() || proc.stopping()) return ran;
        if (!tm->valid) continue;
        if (tm->repeats && tm->interval > 0) {
            const double t = now();
            tm->fireAt += tm->interval;
            if (tm->fireAt < t) tm->fireAt = t + tm->interval;   // skip missed fires, like CFRunLoop
        } else {
            tm->valid = false;
        }
        ran = true;
        tm->fire(c);
    }
    if (frameDue) {
        const double t = now();
        for (auto& h : hooks) {
            if (proc.terminating() || proc.stopping()) return ran;
            h(c, t);
        }
    }
    return ran;
}

void RunLoop::runUntil(CPU::Cpu& c, double until, bool returnAfterSource) {
    for (;;) {
        if (proc.stopping()) throw ProcessExit(0, "stopping");
        if (proc.terminating()) return;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (stopFlag) { stopFlag = false; return; }
        }
        const double remaining = until - now();
        if (remaining <= 0) { runOnce(c, 0); return; }
        const bool ran = runOnce(c, std::min(remaining, 0.1));
        if (ran && returnAfterSource) return;
    }
}

namespace {

struct Loops {
    explicit Loops(Process& p) : main(std::make_unique<RunLoop>(p)), proc(p) {}
    std::unique_ptr<RunLoop> main;
    Process& proc;
    std::mutex mu;
    std::unordered_map<GuestThread*, std::unique_ptr<RunLoop>> perThread;
    std::unordered_map<RunLoop*, id> objects;      // NSRunLoop objects
    Class nsRunLoop = 0, nsDate = 0, dateConcrete = 0, nsTimer = 0, timerConcrete = 0, nsThread = 0, sourceConcrete = 0;
    std::unordered_map<GuestThread*, id> threadObjects;
    struct Perform { id target; SEL sel; id arg; std::shared_ptr<RunLoop::Timer> t; };
    std::vector<Perform> performs;
};
Loops& L(Process& p) { return p.service<Loops>(); }
Loops& L(Ctx& x) { return L(x.proc); }

RunLoop& currentLoop(Ctx& x) {
    GuestThread* t = &x.proc.currentThread();
    if (t == &x.proc.mainThread()) return *L(x).main;
    std::lock_guard<std::mutex> lk(L(x).mu);
    auto& p = L(x).perThread[t];
    if (!p) p = std::make_unique<RunLoop>(x.proc);
    return *p;
}

struct LoopObj : ObjC::HostObject { RunLoop* loop = nullptr; };
id loopObject(Ctx& x, RunLoop& rl) {
    auto& l = L(x);
    {
        std::lock_guard<std::mutex> lk(l.mu);
        auto it = l.objects.find(&rl);
        if (it != l.objects.end()) return it->second;
    }
    const id o = rt(x).createInstance(l.nsRunLoop);
    auto h = std::make_unique<LoopObj>();
    h->loop = &rl;
    rt(x).setHost(o, std::move(h));
    rt(x).makeImmortal(o);
    std::lock_guard<std::mutex> lk(l.mu);
    l.objects[&rl] = o;
    return o;
}
RunLoop& loopOf(Ctx& x, id obj) {
    if (LoopObj* h = obj ? rt(x).hostIf<LoopObj>(obj) : nullptr) return *h->loop;
    return currentLoop(x);
}

double toDouble(uint64_t bits) { double d; std::memcpy(&d, &bits, 8); return d; }

// ---------------------------------------------------------------- NSDate ---

struct DateData : ObjC::HostObject { double t = 0; };
id newDate(Ctx& x, double t, bool owned = false) {
    const id d = rt(x).createInstance(L(x).dateConcrete);
    auto h = std::make_unique<DateData>();
    h->t = t;
    rt(x).setHost(d, std::move(h));
    return owned ? d : autoreleased(x, d);
}
double dateValue(Ctx& x, id d) {
    if (!d) return 0;
    if (DateData* h = rt(x).hostIf<DateData>(d)) return h->t;
    return toDouble(rt(x).send(x.cpu, d, "timeIntervalSinceReferenceDate"));
}
id dt_allocWithZone(Ctx& x, id self, SEL, uint32_t) {
    if (self == L(x).nsDate || self == L(x).dateConcrete) { const id d = rt(x).createInstance(L(x).dateConcrete); rt(x).setHost(d, std::make_unique<DateData>()); return d; }
    return rt(x).createInstance(self);
}
id dt_alloc(Ctx& x, id self, SEL s) { return dt_allocWithZone(x, self, s, 0); }
id dc_date(Ctx& x, id, SEL) { return newDate(x, now()); }
id dc_sinceNow(Ctx& x, id, SEL, double s) { return newDate(x, now() + s); }
id dc_since1970(Ctx& x, id, SEL, double s) { return newDate(x, s - 978307200.0); }
id dc_sinceRef(Ctx& x, id, SEL, double s) { return newDate(x, s); }
id dc_distantFuture(Ctx& x, id, SEL) { return newDate(x, 63113904000.0); }
id dc_distantPast(Ctx& x, id, SEL) { return newDate(x, -63114076800.0); }
double dc_timeIntervalSinceRef(Ctx&, id, SEL) { return now(); }
id di_init(Ctx& x, id self, SEL) { if (DateData* h = rt(x).hostIf<DateData>(self)) h->t = now(); return self; }
id di_initSinceNow(Ctx& x, id self, SEL, double s) { if (DateData* h = rt(x).hostIf<DateData>(self)) h->t = now() + s; return self; }
id di_initSince1970(Ctx& x, id self, SEL, double s) { if (DateData* h = rt(x).hostIf<DateData>(self)) h->t = s - 978307200.0; return self; }
id di_initSinceRef(Ctx& x, id self, SEL, double s) { if (DateData* h = rt(x).hostIf<DateData>(self)) h->t = s; return self; }
id di_initSinceDate(Ctx& x, id self, SEL, double s, id other) { if (DateData* h = rt(x).hostIf<DateData>(self)) h->t = dateValue(x, other) + s; return self; }
double d_sinceRef(Ctx& x, id self, SEL) { return dateValue(x, self); }
double d_sinceNow(Ctx& x, id self, SEL) { return dateValue(x, self) - now(); }
double d_since1970(Ctx& x, id self, SEL) { return dateValue(x, self) + 978307200.0; }
double d_sinceDate(Ctx& x, id self, SEL, id other) { return dateValue(x, self) - dateValue(x, other); }
id d_addingInterval(Ctx& x, id self, SEL, double s) { return newDate(x, dateValue(x, self) + s); }
int32_t d_compare(Ctx& x, id self, SEL, id o) { const double a = dateValue(x, self), b = dateValue(x, o); return a < b ? -1 : a > b ? 1 : 0; }
bool d_isEqualToDate(Ctx& x, id self, SEL, id o) { return o && dateValue(x, self) == dateValue(x, o); }
id d_earlier(Ctx& x, id self, SEL, id o) { return dateValue(x, self) <= dateValue(x, o) ? self : o; }
id d_later(Ctx& x, id self, SEL, id o) { return dateValue(x, self) >= dateValue(x, o) ? self : o; }
uint32_t d_hash(Ctx& x, id self, SEL) { return uint32_t(int64_t(dateValue(x, self))); }
id d_description(Ctx& x, id self, SEL) {
    const time_t t = time_t(dateValue(x, self) + 978307200.0);
    std::tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S +0000", &tm);
    return makeString(x, buf);
}
id d_copy(Ctx& x, id self, SEL, uint32_t) { return rt(x).retain(x.cpu, self); }
void obj_dealloc(Ctx& x, id self, SEL) { rt(x).disposeInstance(x.cpu, self); }

// --------------------------------------------------------------- NSTimer ---

struct TimerData : ObjC::HostObject {
    std::shared_ptr<RunLoop::Timer> t;
    id target = 0, userInfo = 0, block = 0, invocation = 0;
    SEL sel = 0;
    uint32_t callout = 0, info = 0;   // CFRunLoopTimer
    double interval = 0, fireAt = 0;
    bool repeats = false, valid = true;
};
TimerData* td(Ctx& x, id t) { return rt(x).hostIf<TimerData>(t); }

void fireTimer(Ctx& x, id timer) {
    TimerData* d = td(x, timer);
    if (!d || !d->valid) return;
    const bool oneShot = !d->repeats;
    rt(x).retain(x.cpu, timer);
    if (d->block) {
        const uint32_t invoke = x.mem.read<uint32_t>(d->block + 12);
        x.proc.callGuest(x.cpu, invoke, {d->block, timer});
    } else if (d->invocation) {
        rt(x).send(x.cpu, d->invocation, "invoke");
    } else if (d->callout) {
        x.proc.callGuest(x.cpu, d->callout, {timer, d->info});
    } else if (d->target) {
        rt(x).send(x.cpu, d->target, d->sel, {timer});
    }
    if (oneShot) {
        if (TimerData* again = td(x, timer); again && again->valid) {
            again->valid = false;
            const id tgt = again->target, ui = again->userInfo, blk = again->block, inv = again->invocation;
            again->target = again->userInfo = again->block = again->invocation = 0;
            rt(x).release(x.cpu, tgt);
            rt(x).release(x.cpu, ui);
            if (blk) rt(x).release(x.cpu, blk);
            if (inv) rt(x).release(x.cpu, inv);
            rt(x).release(x.cpu, timer);   // the run loop's reference
        }
    }
    rt(x).release(x.cpu, timer);
}

void scheduleTimer(Ctx& x, id timer, RunLoop& rl) {
    TimerData* d = td(x, timer);
    if (!d || !d->valid || d->t) return;
    rt(x).retain(x.cpu, timer);   // the run loop retains scheduled timers
    Process* p = &x.proc;
    d->t = rl.addTimer(d->fireAt - now(), d->interval, d->repeats, [p, timer](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        fireTimer(cx, timer);
    }, timer);
}

id newTimer(Ctx& x, double ti, id target, SEL sel, id userInfo, bool repeats, id block) {
    const id t = rt(x).createInstance(L(x).timerConcrete);
    auto d = std::make_unique<TimerData>();
    d->interval = std::max(ti, 0.0001);
    d->fireAt = now() + std::max(ti, 0.0);
    d->repeats = repeats;
    d->target = rt(x).retain(x.cpu, target);
    d->userInfo = rt(x).retain(x.cpu, userInfo);
    d->sel = sel;
    if (block) d->block = id(rt(x).send(x.cpu, block, "copy"));
    rt(x).setHost(t, std::move(d));
    return t;
}
id newInvocationTimer(Ctx& x, double ti, id invocation, bool repeats) {
    const id t = rt(x).createInstance(L(x).timerConcrete);
    auto d = std::make_unique<TimerData>();
    d->interval = std::max(ti, 0.0001);
    d->fireAt = now() + std::max(ti, 0.0);
    d->repeats = repeats;
    d->invocation = rt(x).retain(x.cpu, invocation);
    rt(x).setHost(t, std::move(d));
    return t;
}
id tc_scheduledInvocation(Ctx& x, id, SEL, double ti, id invocation, bool rep) {
    const id t = newInvocationTimer(x, ti, invocation, rep);
    scheduleTimer(x, t, currentLoop(x));
    return autoreleased(x, t);
}
id tc_timerInvocation(Ctx& x, id, SEL, double ti, id invocation, bool rep) {
    return autoreleased(x, newInvocationTimer(x, ti, invocation, rep));
}
id tc_scheduled(Ctx& x, id, SEL, double ti, id target, SEL sel, id ui, bool rep) {
    const id t = newTimer(x, ti, target, sel, ui, rep, 0);
    scheduleTimer(x, t, currentLoop(x));
    return autoreleased(x, t);
}
id tc_timer(Ctx& x, id, SEL, double ti, id target, SEL sel, id ui, bool rep) { return autoreleased(x, newTimer(x, ti, target, sel, ui, rep, 0)); }
id tc_scheduledBlock(Ctx& x, id, SEL, double ti, bool rep, id block) {
    const id t = newTimer(x, ti, 0, 0, 0, rep, block);
    scheduleTimer(x, t, currentLoop(x));
    return autoreleased(x, t);
}
id tc_timerBlock(Ctx& x, id, SEL, double ti, bool rep, id block) { return autoreleased(x, newTimer(x, ti, 0, 0, 0, rep, block)); }
id ti_initWithFireDate(Ctx& x, id self, SEL, id date, double ti, id target, SEL sel, id ui, bool rep) {
    rt(x).disposeInstance(x.cpu, self);
    const id t = newTimer(x, ti, target, sel, ui, rep, 0);
    td(x, t)->fireAt = dateValue(x, date);
    return t;
}
void t_invalidate(Ctx& x, id self, SEL) {
    TimerData* d = td(x, self);
    if (!d || !d->valid) return;
    d->valid = false;
    if (d->t) d->t->valid = false;
    const bool scheduled = d->t != nullptr;
    const id tgt = d->target, ui = d->userInfo, blk = d->block, inv = d->invocation;
    d->target = d->userInfo = d->block = d->invocation = 0;
    rt(x).release(x.cpu, tgt);
    rt(x).release(x.cpu, ui);
    if (blk) rt(x).release(x.cpu, blk);
    if (inv) rt(x).release(x.cpu, inv);
    if (scheduled) rt(x).release(x.cpu, self);
}
void t_fire(Ctx& x, id self, SEL) {
    TimerData* d = td(x, self);
    if (!d || !d->valid) return;
    if (d->repeats) fireTimer(x, self);
    else { fireTimer(x, self); }
}
bool t_isValid(Ctx& x, id self, SEL) { TimerData* d = td(x, self); return d && d->valid; }
id t_userInfo(Ctx& x, id self, SEL) { TimerData* d = td(x, self); return d ? d->userInfo : 0; }
double t_timeInterval(Ctx& x, id self, SEL) { TimerData* d = td(x, self); return d && d->repeats ? d->interval : 0; }
id t_fireDate(Ctx& x, id self, SEL) { TimerData* d = td(x, self); return newDate(x, d ? (d->t ? d->t->fireAt : d->fireAt) : 0); }
void t_setFireDate(Ctx& x, id self, SEL, id date) {
    TimerData* d = td(x, self);
    if (!d) return;
    d->fireAt = dateValue(x, date);
    if (d->t) d->t->fireAt = d->fireAt;
}
void timer_dealloc(Ctx& x, id self, SEL) {
    if (TimerData* d = td(x, self)) {
        if (d->t) d->t->valid = false;
        const id tgt = d->target, ui = d->userInfo, blk = d->block, inv = d->invocation;
        d->target = d->userInfo = d->block = d->invocation = 0;
        rt(x).release(x.cpu, tgt);
        rt(x).release(x.cpu, ui);
        if (blk) rt(x).release(x.cpu, blk);
        if (inv) rt(x).release(x.cpu, inv);
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------------------- NSRunLoop ---

id rc_current(Ctx& x, id, SEL) { return loopObject(x, currentLoop(x)); }
id rc_main(Ctx& x, id, SEL) { return loopObject(x, *L(x).main); }
void r_addTimer(Ctx& x, id self, SEL, id timer, id) { scheduleTimer(x, timer, loopOf(x, self)); }
void r_run(Ctx& x, id self, SEL) { while (!x.proc.terminating()) loopOf(x, self).runUntil(x.cpu, 1e18); }
void r_runUntilDate(Ctx& x, id self, SEL, id date) { loopOf(x, self).runUntil(x.cpu, dateValue(x, date)); }
bool r_runModeBeforeDate(Ctx& x, id self, SEL, id, id date) { loopOf(x, self).runUntil(x.cpu, dateValue(x, date), true); return true; }
id r_currentMode(Ctx& x, id, SEL) { return constString(x.proc, "kCFDefaultRunLoopMode"); }
uint32_t r_getCFRunLoop(Ctx&, id self, SEL) { return self; }

// CFRunLoop
uint32_t cf_getCurrent(Ctx& x) { return loopObject(x, currentLoop(x)); }
uint32_t cf_getMain(Ctx& x) { return loopObject(x, *L(x).main); }
void cf_run(Ctx& x) { currentLoop(x).runUntil(x.cpu, 1e18); }
int32_t cf_runInMode(Ctx& x, uint32_t, double seconds, bool returnAfterSource) {
    RunLoop& rl = currentLoop(x);
    const double until = now() + std::max(0.0, seconds);
    for (;;) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (x.proc.terminating()) return 2;   // kCFRunLoopRunStopped
        {
            std::lock_guard<std::mutex> lk(rl.mu);
            if (rl.stopFlag) { rl.stopFlag = false; return 2; }   // kCFRunLoopRunStopped
        }
        const double remaining = until - now();
        const bool ran = rl.runOnce(x.cpu, std::max(0.0, std::min(remaining, 0.1)));
        if (ran && returnAfterSource) return 4;                  // kCFRunLoopRunHandledSource
        if (now() >= until) return 3;                            // kCFRunLoopRunTimedOut
    }
}
void cf_stop(Ctx& x, uint32_t rl) { loopOf(x, rl).stop(); }
void cf_wakeUp(Ctx& x, uint32_t rl) { loopOf(x, rl).cv.notify_all(); }
void cf_addTimer(Ctx& x, uint32_t rl, id timer, uint32_t) { scheduleTimer(x, timer, loopOf(x, rl)); }
void cf_removeTimer(Ctx& x, uint32_t, id timer, uint32_t) { t_invalidate(x, timer, 0); }
uint32_t cf_timerCreate(Ctx& x, uint32_t, double fireDate, double interval, uint32_t, int32_t, uint32_t callout, GuestPtr context) {
    const id t = newTimer(x, interval, 0, 0, 0, interval > 0, 0);
    TimerData* d = td(x, t);
    d->fireAt = fireDate;
    d->callout = callout;
    d->info = context ? x.mem.read<uint32_t>(context + 4) : 0;
    return t;
}
void cf_timerInvalidate(Ctx& x, id t) { t_invalidate(x, t, 0); }
void cf_timerSetNextFireDate(Ctx& x, id t, double date) { if (TimerData* d = td(x, t)) { d->fireAt = date; if (d->t) d->t->fireAt = date; } }
double cf_absoluteTime(Ctx&) { return now(); }

// ------------------------------------------------- CFRunLoopSource ---
// Version-0 (custom) sources only. Version-1 sources wrap a Mach port, which this
// runtime has no equivalent of, so creating one fails visibly instead of pretending.

struct SourceData : ObjC::HostObject {
    std::shared_ptr<RunLoop::Source> src = std::make_shared<RunLoop::Source>();
    uint32_t info = 0, releaseCb = 0, scheduleCb = 0, cancelCb = 0, performCb = 0;
    struct Added { RunLoop* loop; uint32_t mode; std::string name; };   // mode: retained CFString; modes compare by `name`
    std::mutex mu;               // guards `loops` and the add/invalidate ordering; never held across a guest callback or a release
    std::vector<Added> loops;    // one entry per (run loop, mode) membership
};
// Run loop modes are compared by string value, as CoreFoundation does, not by CFString address.
std::string modeKey(Ctx& x, uint32_t mode) {
    if (!mode) return {};
    if (Foundation::isString(x, mode)) return "s:" + Foundation::utf8(x, mode);
    return "p:" + std::to_string(mode);
}
SourceData* sd(Ctx& x, id s) { return s ? rt(x).hostIf<SourceData>(s) : nullptr; }

// Drop the (loop, mode) memberships selected by `match`: each gets a cancel callback; the loop itself
// lets go of the source (and its reference) only when its last mode is gone. Callbacks run unlocked.
template <class Match>
void detachSources(Ctx& x, id s, SourceData& d, Match match) {
    std::vector<SourceData::Added> gone;
    std::vector<RunLoop*> released;
    {
        std::lock_guard<std::mutex> lk(d.mu);
        for (size_t i = 0; i < d.loops.size();) {
            if (!match(d.loops[i])) { ++i; continue; }
            gone.push_back(d.loops[i]);
            d.loops.erase(d.loops.begin() + std::ptrdiff_t(i));
        }
        for (const auto& a : gone) {
            const bool stillIn = std::any_of(d.loops.begin(), d.loops.end(), [&](const SourceData::Added& o) { return o.loop == a.loop; });
            if (!stillIn && std::find(released.begin(), released.end(), a.loop) == released.end()) released.push_back(a.loop);
        }
    }
    for (RunLoop* l : released) l->removeSource(d.src);
    for (const auto& a : gone) {
        if (d.cancelCb) x.proc.callGuest(x.cpu, d.cancelCb, {d.info, loopObject(x, *a.loop), a.mode});
        if (a.mode && Foundation::isString(x, a.mode)) rt(x).release(x.cpu, a.mode);   // the membership's own reference to the mode string
    }
    for (size_t i = 0; i < released.size(); ++i) rt(x).release(x.cpu, s);   // one reference per loop that held the source
}

id cf_sourceCreate(Ctx& x, uint32_t, int32_t order, GuestPtr context) {
    if (!context) {
        x.proc.log(LogLevel::Warning, "CFRunLoopSourceCreate: NULL context; no source created");
        return 0;
    }
    const uint32_t version = x.mem.read<uint32_t>(context);
    if (version != 0) {
        x.proc.logf(LogLevel::Warning, "CFRunLoopSourceCreate: version-%u (Mach port) sources aren't supported; no source created", version);
        return 0;
    }
    // CFRunLoopSourceContext (32-bit): version, info, retain, release, copyDescription, equal, hash, schedule, cancel, perform
    const id obj = rt(x).createInstance(L(x).sourceConcrete);
    auto d = std::make_unique<SourceData>();
    uint32_t info = x.mem.read<uint32_t>(context + 4);
    const uint32_t retainCb = x.mem.read<uint32_t>(context + 8);
    if (retainCb) info = uint32_t(x.proc.callGuest(x.cpu, retainCb, {info}));
    d->info = info;
    d->releaseCb = x.mem.read<uint32_t>(context + 12);
    d->scheduleCb = x.mem.read<uint32_t>(context + 28);
    d->cancelCb = x.mem.read<uint32_t>(context + 32);
    d->performCb = x.mem.read<uint32_t>(context + 36);
    d->src->order = order;
    Process* p = &x.proc;
    d->src->perform = [p, info, perform = d->performCb](CPU::Cpu& c) {
        if (!perform) return;
        Ctx cx{*p, c, p->mem()};
        p->callGuest(c, perform, {info});
    };
    rt(x).setHost(obj, std::move(d));
    return obj;
}
void cf_addSource(Ctx& x, uint32_t rl, id s, uint32_t mode) {
    SourceData* d = sd(x, s);
    if (!d) return;
    RunLoop& loop = loopOf(x, rl);
    const std::string modeName = modeKey(x, mode);
    {
        // Validity, membership and attachment are one step under d->mu, so an Invalidate on another thread
        // either runs first (and the add is refused) or runs after (and detaches what was just attached).
        // detachSources drops d->mu before it touches the loop, so this order cannot deadlock.
        std::lock_guard<std::mutex> lk(d->mu);
        if (!d->src->valid.load()) return;
        for (const auto& a : d->loops) if (a.loop == &loop && a.name == modeName) return;   // already scheduled in this mode
        const bool firstForLoop = std::none_of(d->loops.begin(), d->loops.end(), [&](const SourceData::Added& a) { return a.loop == &loop; });
        if (mode && Foundation::isString(x, mode)) rt(x).retain(x.cpu, mode);   // held for the membership's lifetime; released after the cancel callback
        d->loops.push_back({&loop, mode, modeName});
        if (firstForLoop) {
            rt(x).retain(x.cpu, s);   // the run loop retains its sources (once per loop, however many modes)
            loop.addSource(d->src);
        }
    }
    if (d->scheduleCb) x.proc.callGuest(x.cpu, d->scheduleCb, {d->info, loopObject(x, loop), mode});
}
void cf_removeSource(Ctx& x, uint32_t rl, id s, uint32_t mode) {
    SourceData* d = sd(x, s);
    if (!d) return;
    RunLoop* loop = &loopOf(x, rl);
    const std::string modeName = modeKey(x, mode);
    detachSources(x, s, *d, [&](const SourceData::Added& a) { return a.loop == loop && a.name == modeName; });
}
bool cf_containsSource(Ctx& x, uint32_t rl, id s, uint32_t mode) {
    SourceData* d = sd(x, s);
    if (!d || !d->src->valid.load()) return false;
    RunLoop* loop = &loopOf(x, rl);
    const std::string modeName = modeKey(x, mode);
    std::lock_guard<std::mutex> lk(d->mu);
    return std::any_of(d->loops.begin(), d->loops.end(), [&](const SourceData::Added& a) { return a.loop == loop && a.name == modeName; });
}
void cf_sourceSignal(Ctx& x, id s) {
    SourceData* d = sd(x, s);
    if (!d || !d->src->valid.load()) return;
    d->src->signalled.store(true);
    std::vector<RunLoop*> wake;
    {
        std::lock_guard<std::mutex> lk(d->mu);
        for (const auto& a : d->loops) if (std::find(wake.begin(), wake.end(), a.loop) == wake.end()) wake.push_back(a.loop);
    }
    for (RunLoop* l : wake) l->wake();   // unlocked: wake() may take the loop's own lock
}
void cf_sourceInvalidate(Ctx& x, id s) {
    SourceData* d = sd(x, s);
    if (!d || !d->src->valid.exchange(false)) return;
    rt(x).retain(x.cpu, s);   // keep it alive while the loops let go of it
    detachSources(x, s, *d, [](const SourceData::Added&) { return true; });
    rt(x).release(x.cpu, s);
}
bool cf_sourceIsValid(Ctx& x, id s) { SourceData* d = sd(x, s); return d && d->src->valid.load(); }
int32_t cf_sourceGetOrder(Ctx& x, id s) { SourceData* d = sd(x, s); return d ? int32_t(d->src->order) : 0; }
void source_dealloc(Ctx& x, id self, SEL) {
    if (SourceData* d = sd(x, self)) {
        d->src->valid.store(false);
        {
            std::lock_guard<std::mutex> lk(d->mu);
            for (const auto& a : d->loops) a.loop->removeSource(d->src);   // can't happen while a loop holds a reference
        }
        if (d->releaseCb) x.proc.callGuest(x.cpu, d->releaseCb, {d->info});
    }
    rt(x).disposeInstance(x.cpu, self);
}

// ------------------------------------------------- NSObject performs ---

void performLater(Ctx& x, id target, SEL sel, id arg, double delay) {
    auto& l = L(x);
    rt(x).retain(x.cpu, target);
    rt(x).retain(x.cpu, arg);
    Process* p = &x.proc;
    auto entry = std::make_shared<std::shared_ptr<RunLoop::Timer>>();
    auto t = currentLoop(x).addTimer(delay, 0, false, [p, target, sel, arg, entry](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        {
            auto& ll = L(*p);
            std::lock_guard<std::mutex> lk(ll.mu);
            ll.performs.erase(std::remove_if(ll.performs.begin(), ll.performs.end(), [&](auto& pr) { return pr.t == *entry; }), ll.performs.end());
        }
        rt(cx).send(c, target, sel, {arg});
        rt(cx).release(c, arg);
        rt(cx).release(c, target);
    });
    *entry = t;
    std::lock_guard<std::mutex> lk(l.mu);
    l.performs.push_back({target, sel, arg, t});
}
void o_performAfterDelay(Ctx& x, id self, SEL, SEL sel, id arg, double delay) { performLater(x, self, sel, arg, delay); }
void o_performAfterDelayModes(Ctx& x, id self, SEL, SEL sel, id arg, double delay, id) { performLater(x, self, sel, arg, delay); }
void cancelPerforms(Ctx& x, id target, std::function<bool(const Loops::Perform&)> match) {
    std::vector<Loops::Perform> cancelled;
    {
        auto& l = L(x);
        std::lock_guard<std::mutex> lk(l.mu);
        for (auto it = l.performs.begin(); it != l.performs.end();) {
            if (it->target == target && match(*it)) { it->t->valid = false; cancelled.push_back(*it); it = l.performs.erase(it); }
            else ++it;
        }
    }
    for (auto& pr : cancelled) { rt(x).release(x.cpu, pr.arg); rt(x).release(x.cpu, pr.target); }
}
void oc_cancelAll(Ctx& x, id, SEL, id target) { cancelPerforms(x, target, [](auto&) { return true; }); }
void oc_cancelSel(Ctx& x, id, SEL, id target, SEL sel, id arg) {
    cancelPerforms(x, target, [&](const Loops::Perform& pr) { return pr.sel == sel && objectsEqual(x, pr.arg, arg); });
}
void o_performOnMain(Ctx& x, id self, SEL, SEL sel, id arg, bool wait) {
    const bool onMain = &x.proc.currentThread() == &x.proc.mainThread();
    if (onMain && wait) { rt(x).send(x.cpu, self, sel, {arg}); return; }
    rt(x).retain(x.cpu, self);
    rt(x).retain(x.cpu, arg);
    Process* p = &x.proc;
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    L(x).main->post([p, self, sel, arg, done](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        try { rt(cx).send(c, self, sel, {arg}); } catch (...) { done->set_value(); throw; }
        rt(cx).release(c, arg);
        rt(cx).release(c, self);
        done->set_value();
    });
    if (wait && !onMain) {
        while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    }
}
void o_performOnMainModes(Ctx& x, id self, SEL s, SEL sel, id arg, bool wait, id) { o_performOnMain(x, self, s, sel, arg, wait); }

struct ThreadData : ObjC::HostObject {
    GuestThread* t = nullptr;
    id target = 0, arg = 0, name = 0, dict = 0;
    SEL sel = 0;
    bool cancelled = false;
    std::atomic<bool> started{false}, finished{false};   // -isExecuting / -isFinished
};
void bindThreadObject(Ctx& x, id threadObj, GuestThread* t);
// background performs run on a new guest thread through a host trampoline
void perform_trampoline(Ctx& x, GuestPtr req) {
    const id target = x.mem.read<uint32_t>(req), arg = x.mem.read<uint32_t>(req + 8);
    const SEL sel = x.mem.read<uint32_t>(req + 4);
    const id threadObj = x.mem.read<uint32_t>(req + 12);
    x.proc.heap().free(req);
    if (threadObj) bindThreadObject(x, threadObj, &x.proc.currentThread());
    struct MarkFinished {
        Ctx& x; id o;
        ~MarkFinished() { if (o) if (ThreadData* d = rt(x).hostIf<ThreadData>(o)) d->finished = true; }
    } mark{x, threadObj};
    const uint32_t pool = rt(x).poolPush();
    rt(x).send(x.cpu, target, sel, {arg});
    rt(x).poolPop(x.cpu, pool);
    rt(x).release(x.cpu, arg);
    rt(x).release(x.cpu, target);
}
void startBackground(Ctx& x, id target, SEL sel, id arg, id threadObj = 0) {
    const uint32_t req = x.proc.heap().alloc(16);
    x.mem.write<uint32_t>(req, rt(x).retain(x.cpu, target));
    x.mem.write<uint32_t>(req + 4, sel);
    x.mem.write<uint32_t>(req + 8, rt(x).retain(x.cpu, arg));
    x.mem.write<uint32_t>(req + 12, threadObj);
    x.proc.startThread(*x.proc.lookupSymbol("__hvm_perform_trampoline"), req, 512u << 10, true);
}
void o_performInBackground(Ctx& x, id self, SEL, SEL sel, id arg) { startBackground(x, self, sel, arg); }

// -------------------------------------------------------------- NSThread ---

id threadObject(Ctx& x, GuestThread* t) {
    auto& l = L(x);
    {
        std::lock_guard<std::mutex> lk(l.mu);
        auto it = l.threadObjects.find(t);
        if (it != l.threadObjects.end()) return it->second;
    }
    const id o = rt(x).createInstance(l.nsThread);
    auto h = std::make_unique<ThreadData>();
    h->t = t;
    h->started = true;
    rt(x).setHost(o, std::move(h));
    rt(x).makeImmortal(o);
    std::lock_guard<std::mutex> lk(l.mu);
    l.threadObjects[t] = o;
    return o;
}
// An NSThread the app created and started: [NSThread currentThread] on that thread returns it.
void bindThreadObject(Ctx& x, id threadObj, GuestThread* t) {
    ThreadData* d = rt(x).hostIf<ThreadData>(threadObj);
    if (!d) return;
    auto& l = L(x);
    std::lock_guard<std::mutex> lk(l.mu);
    d->t = t;
    l.threadObjects[t] = threadObj;
}
RunLoop& loopForThread(Ctx& x, GuestThread* t) {
    if (t == &x.proc.mainThread()) return *L(x).main;
    std::lock_guard<std::mutex> lk(L(x).mu);
    auto& p = L(x).perThread[t];
    if (!p) p = std::make_unique<RunLoop>(x.proc);
    return *p;
}
// performSelector:onThread:withObject:waitUntilDone:[modes:]
void o_performOnThread(Ctx& x, id self, SEL, SEL sel, id thread, id arg, bool wait) {
    ThreadData* d = thread ? rt(x).hostIf<ThreadData>(thread) : nullptr;
    for (int i = 0; d && !d->t && i < 100 && !x.proc.stopping(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    GuestThread* t = d && d->t ? d->t : &x.proc.mainThread();
    if (t == &x.proc.currentThread() && wait) { rt(x).send(x.cpu, self, sel, {arg}); return; }
    rt(x).retain(x.cpu, self);
    rt(x).retain(x.cpu, arg);
    Process* p = &x.proc;
    auto done = std::make_shared<std::promise<void>>();
    auto fut = done->get_future();
    loopForThread(x, t).post([p, self, sel, arg, done](CPU::Cpu& c) {
        Ctx cx{*p, c, p->mem()};
        try { rt(cx).send(c, self, sel, {arg}); } catch (...) { done->set_value(); throw; }
        rt(cx).release(c, arg);
        rt(cx).release(c, self);
        done->set_value();
    });
    if (wait && t != &x.proc.currentThread()) {
        while (fut.wait_for(std::chrono::milliseconds(50)) != std::future_status::ready)
            if (x.proc.stopping()) throw ProcessExit(0, "stopping");
    }
}
void o_performOnThreadModes(Ctx& x, id self, SEL s, SEL sel, id thread, id arg, bool wait, id) { o_performOnThread(x, self, s, sel, thread, arg, wait); }
// NSPort / NSMachPort: AFNetworking-style "keep this run loop alive" ports. Nothing arrives on them.
id port_new(Ctx& x, id cls, SEL) { return autoreleased(x, id(rt(x).send(x.cpu, id(rt(x).send(x.cpu, cls, rt(x).selAlloc)), "init"))); }
void rl_portNoop(Ctx&, id, SEL, id, id) {}

id th_current(Ctx& x, id, SEL) { return threadObject(x, &x.proc.currentThread()); }
id th_main(Ctx& x, id, SEL) { return threadObject(x, &x.proc.mainThread()); }
bool th_isMainClass(Ctx& x, id, SEL) { return &x.proc.currentThread() == &x.proc.mainThread(); }
bool th_isMain(Ctx& x, id self, SEL) { ThreadData* d = rt(x).hostIf<ThreadData>(self); return d && d->t == &x.proc.mainThread(); }
bool th_isMultiThreaded(Ctx&, id, SEL) { return true; }
void th_sleepFor(Ctx& x, id, SEL, double s) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(std::max(0.0, s)));
    while (std::chrono::steady_clock::now() < end) {
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(end - std::chrono::steady_clock::now(), std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::milliseconds(50))));
    }
}
void th_sleepUntil(Ctx& x, id self, SEL s, id date) { th_sleepFor(x, self, s, dateValue(x, date) - now()); }
void th_detach(Ctx& x, id, SEL, SEL sel, id target, id arg) { startBackground(x, target, sel, arg); }
id th_initWithTarget(Ctx& x, id self, SEL, id target, SEL sel, id arg) {
    auto h = std::make_unique<ThreadData>();
    h->target = rt(x).retain(x.cpu, target);
    h->sel = sel;
    h->arg = rt(x).retain(x.cpu, arg);
    rt(x).setHost(self, std::move(h));
    return self;
}
void th_start(Ctx& x, id self, SEL) {
    ThreadData* d = rt(x).hostIf<ThreadData>(self);
    if (d) {
        if (d->started.exchange(true)) return;   // NSInvalidArgumentException on iOS; starting twice is a no-op here
    }
    if (d && d->target) startBackground(x, d->target, d->sel, d->arg, self);
    else if (d) startBackground(x, self, rt(x).sel("main"), 0, self);
}
void th_noop(Ctx&, id, SEL) {}
id th_name(Ctx& x, id self, SEL) { ThreadData* d = rt(x).hostIf<ThreadData>(self); return d && d->name ? d->name : makeString(x, ""); }
void th_setName(Ctx& x, id self, SEL, id n) { if (ThreadData* d = rt(x).hostIf<ThreadData>(self)) { const id old = d->name; d->name = id(rt(x).send(x.cpu, n, "copy")); rt(x).release(x.cpu, old); } }
id th_dict(Ctx& x, id self, SEL) {
    ThreadData* d = rt(x).hostIf<ThreadData>(self);
    if (!d) return 0;
    if (!d->dict) d->dict = makeDict(x, {}, true, true);
    return d->dict;
}
void th_cancel(Ctx& x, id self, SEL) { if (ThreadData* d = rt(x).hostIf<ThreadData>(self)) d->cancelled = true; }
bool th_isCancelled(Ctx& x, id self, SEL) { ThreadData* d = rt(x).hostIf<ThreadData>(self); return d && d->cancelled; }
bool th_isExecuting(Ctx& x, id self, SEL) { ThreadData* d = rt(x).hostIf<ThreadData>(self); return d && d->started && !d->finished; }
bool th_isFinished(Ctx& x, id self, SEL) { ThreadData* d = rt(x).hostIf<ThreadData>(self); return d && d->finished; }
double th_priority(Ctx&, id, SEL) { return 0.5; }
bool th_setPriority(Ctx&, id, SEL, double) { return true; }

} // namespace

RunLoop& mainRunLoop(Process& p) { return *L(p).main; }
double dateInterval(Ctx& x, id date) { return dateValue(x, date); }
id makeDate(Ctx& x, double t, bool owned) { return newDate(x, t, owned); }

void registerRunLoop(Process& p, ObjC::Runtime& r) {
    auto& l = L(p);
    ObjC::ClassBuilder date = r.defineClass("NSDate", "NSObject");
    ObjC::ClassBuilder dateC = r.defineClass("__NSDate", "NSDate");
    l.nsDate = date.cls(); l.dateConcrete = dateC.cls();
    date.cm<&dt_alloc>("alloc").cm<&dt_allocWithZone>("allocWithZone:").cm<&dc_date>("date")
        .cm<&dc_sinceNow>("dateWithTimeIntervalSinceNow:").cm<&dc_since1970>("dateWithTimeIntervalSince1970:")
        .cm<&dc_sinceRef>("dateWithTimeIntervalSinceReferenceDate:").cm<&dc_distantFuture>("distantFuture")
        .cm<&dc_distantPast>("distantPast").cm<&dc_timeIntervalSinceRef>("timeIntervalSinceReferenceDate", "d@:");
    date.m<&di_init>("init").m<&di_initSinceNow>("initWithTimeIntervalSinceNow:").m<&di_initSince1970>("initWithTimeIntervalSince1970:")
        .m<&di_initSinceRef>("initWithTimeIntervalSinceReferenceDate:").m<&di_initSinceDate>("initWithTimeInterval:sinceDate:")
        .m<&d_sinceRef>("timeIntervalSinceReferenceDate", "d@:").m<&d_sinceNow>("timeIntervalSinceNow", "d@:")
        .m<&d_since1970>("timeIntervalSince1970", "d@:").m<&d_sinceDate>("timeIntervalSinceDate:", "d@:@")
        .m<&d_addingInterval>("dateByAddingTimeInterval:").m<&d_addingInterval>("addTimeInterval:")
        .m<&d_compare>("compare:", "i@:@").m<&d_isEqualToDate>("isEqualToDate:", "c@:@").m<&d_isEqualToDate>("isEqual:", "c@:@")
        .m<&d_earlier>("earlierDate:").m<&d_later>("laterDate:").m<&d_hash>("hash", "I@:").m<&d_description>("description")
        .m<&d_copy>("copyWithZone:");
    dateC.m<&obj_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder sourceC = r.defineClass("__NSCFRunLoopSource", "NSObject");
    l.sourceConcrete = sourceC.cls();
    sourceC.m<&source_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder timer = r.defineClass("NSTimer", "NSObject");
    ObjC::ClassBuilder timerC = r.defineClass("__NSCFTimer", "NSTimer");
    l.nsTimer = timer.cls(); l.timerConcrete = timerC.cls();
    timer.cm<&tc_scheduled>("scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:")
         .cm<&tc_timer>("timerWithTimeInterval:target:selector:userInfo:repeats:")
         .cm<&tc_scheduledInvocation>("scheduledTimerWithTimeInterval:invocation:repeats:")
         .cm<&tc_timerInvocation>("timerWithTimeInterval:invocation:repeats:")
         .cm<&tc_scheduledBlock>("scheduledTimerWithTimeInterval:repeats:block:")
         .cm<&tc_timerBlock>("timerWithTimeInterval:repeats:block:");
    timer.m<&ti_initWithFireDate>("initWithFireDate:interval:target:selector:userInfo:repeats:")
         .m<&t_invalidate>("invalidate", "v@:").m<&t_fire>("fire", "v@:").m<&t_isValid>("isValid", "c@:")
         .m<&t_userInfo>("userInfo").m<&t_timeInterval>("timeInterval", "d@:").m<&t_fireDate>("fireDate")
         .m<&t_setFireDate>("setFireDate:");
    timerC.m<&timer_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder rl = r.defineClass("NSRunLoop", "NSObject");
    l.nsRunLoop = rl.cls();
    rl.cm<&rc_current>("currentRunLoop").cm<&rc_main>("mainRunLoop");
    rl.m<&r_addTimer>("addTimer:forMode:").m<&r_run>("run", "v@:").m<&r_runUntilDate>("runUntilDate:")
      .m<&r_runModeBeforeDate>("runMode:beforeDate:").m<&r_currentMode>("currentMode").m<&r_getCFRunLoop>("getCFRunLoop");

    ObjC::ClassBuilder obj = r.builder(r.nsObject);
    obj.m<&o_performAfterDelay>("performSelector:withObject:afterDelay:")
       .m<&o_performAfterDelayModes>("performSelector:withObject:afterDelay:inModes:")
       .m<&o_performOnMain>("performSelectorOnMainThread:withObject:waitUntilDone:")
       .m<&o_performOnMainModes>("performSelectorOnMainThread:withObject:waitUntilDone:modes:")
       .m<&o_performOnThread>("performSelector:onThread:withObject:waitUntilDone:")
       .m<&o_performOnThreadModes>("performSelector:onThread:withObject:waitUntilDone:modes:")
       .m<&o_performInBackground>("performSelectorInBackground:withObject:")
       .cm<&oc_cancelAll>("cancelPreviousPerformRequestsWithTarget:")
       .cm<&oc_cancelSel>("cancelPreviousPerformRequestsWithTarget:selector:object:");
    p.def<&perform_trampoline>("__hvm_perform_trampoline");

    ObjC::ClassBuilder port = r.defineClass("NSPort", "NSObject");
    port.cm<&port_new>("port");
    r.defineClass("NSMachPort", "NSPort");
    r.builder(l.nsRunLoop).m<&rl_portNoop>("addPort:forMode:", "v@:@@").m<&rl_portNoop>("removePort:forMode:", "v@:@@");

    ObjC::ClassBuilder th = r.defineClass("NSThread", "NSObject");
    l.nsThread = th.cls();
    th.cm<&th_current>("currentThread").cm<&th_main>("mainThread").cm<&th_isMainClass>("isMainThread", "c@:")
      .cm<&th_isMultiThreaded>("isMultiThreaded", "c@:").cm<&th_sleepFor>("sleepForTimeInterval:")
      .cm<&th_sleepUntil>("sleepUntilDate:").cm<&th_detach>("detachNewThreadSelector:toTarget:withObject:")
      .cm<&th_priority>("threadPriority", "d@:").cm<&th_setPriority>("setThreadPriority:", "c@:d");
    th.m<&th_initWithTarget>("initWithTarget:selector:object:").m<&th_start>("start", "v@:").m<&th_isMain>("isMainThread", "c@:")
      .m<&th_noop>("main", "v@:").m<&th_name>("name").m<&th_setName>("setName:").m<&th_dict>("threadDictionary")
      .m<&th_cancel>("cancel", "v@:").m<&th_isCancelled>("isCancelled", "c@:")
      .m<&th_isExecuting>("isExecuting", "c@:").m<&th_isFinished>("isFinished", "c@:")
      .m<&th_priority>("threadPriority", "d@:").m<&th_setPriority>("setThreadPriority:", "c@:d");

    p.def<&cf_getCurrent>("_CFRunLoopGetCurrent");
    p.def<&cf_getMain>("_CFRunLoopGetMain");
    p.def<&cf_run>("_CFRunLoopRun");
    p.def<&cf_runInMode>("_CFRunLoopRunInMode");
    p.def<&cf_stop>("_CFRunLoopStop");
    p.def<&cf_wakeUp>("_CFRunLoopWakeUp");
    p.def<&cf_addTimer>("_CFRunLoopAddTimer");
    p.def<&cf_removeTimer>("_CFRunLoopRemoveTimer");
    p.def<&cf_timerCreate>("_CFRunLoopTimerCreate");
    p.def<&cf_timerInvalidate>("_CFRunLoopTimerInvalidate");
    p.def<&cf_timerSetNextFireDate>("_CFRunLoopTimerSetNextFireDate");
    p.def<&cf_sourceCreate>("_CFRunLoopSourceCreate");
    p.def<&cf_addSource>("_CFRunLoopAddSource");
    p.def<&cf_removeSource>("_CFRunLoopRemoveSource");
    p.def<&cf_containsSource>("_CFRunLoopContainsSource");
    p.def<&cf_sourceSignal>("_CFRunLoopSourceSignal");
    p.def<&cf_sourceInvalidate>("_CFRunLoopSourceInvalidate");
    p.def<&cf_sourceIsValid>("_CFRunLoopSourceIsValid");
    p.def<&cf_sourceGetOrder>("_CFRunLoopSourceGetOrder");
    p.def<&cf_absoluteTime>("_CFAbsoluteTimeGetCurrent");
    for (const char* n : {"_kCFRunLoopDefaultMode", "_kCFRunLoopCommonModes", "_NSDefaultRunLoopMode", "_NSRunLoopCommonModes",
                          "_UITrackingRunLoopMode", "_NSEventTrackingRunLoopMode"}) {
        const uint32_t cell = p.defineDataBlock(n, 4);
        p.mem().write<uint32_t>(cell, constString(p, n + 1));
    }
}

RunLoop& runLoopForObject(Ctx& x, id loop) { return loopOf(x, loop); }

} // namespace Hydrogen::Runtime::Foundation
