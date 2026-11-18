// HydrogenVM — libandroid's event plumbing: ALooper, AInputQueue / AInputEvent / AMotionEvent /
// AKeyEvent, and ASensorManager / ASensorEventQueue.
//
// A looper belongs to the thread that prepared it. Its sources are the input queue and sensor
// event queues attached to it, each with an ident (returned by ALooper_pollAll when it has
// events) or a callback (called from the poll). ALooper_wake ends a poll early.
//
// Input events are guest memory blocks the game reads only through the AInputEvent accessors;
// HydrogenVM keeps their contents host-side. A driver (NativeActivity) feeds touches with
// pushTouchEvent. Sensors: one accelerometer, reporting the device lying flat.
#include "runtime/android/Android.hpp"
#include "runtime/android/BionicInternal.hpp"
#include "runtime/Support.hpp"
#include "core/cpu/Engine.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Hydrogen::Runtime::Android {
namespace {

using clock = std::chrono::steady_clock;
constexpr int32_t kPollWake = -1, kPollCallback = -2, kPollTimeout = -3;

struct Source { int32_t fd = -1, ident = 0; uint32_t callback = 0, data = 0; uint32_t object = 0; };
struct Looper { std::vector<Source> sources; bool woken = false; };
struct Event {
    int32_t type = 2;                    // AINPUT_EVENT_TYPE_MOTION (1: key)
    int32_t action = 0, source = 0x1002; // SOURCE_TOUCHSCREEN
    int32_t keyCode = 0, metaState = 0, deviceId = 1;
    int64_t downTime = 0, eventTime = 0;
    std::vector<int32_t> ids;
    std::vector<float> xs, ys;
    float joyX = 0, joyY = 0, joyZ = 0, joyRz = 0, joyHatX = 0, joyHatY = 0;
};
struct ActivePointer { int32_t id; float x, y; };
struct SensorQueue {
    uint32_t looper = 0;
    bool accelEnabled = false;
    int64_t periodNs = 16'666'667;
    clock::time_point last{};
};

struct Plumbing {
    explicit Plumbing(Process&) {}
    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<uint32_t, Looper> loopers;                 // ALooper* -> state
    std::unordered_map<std::thread::id, uint32_t> threadLooper;
    std::unordered_map<uint32_t, Event> events;                   // AInputEvent* -> contents
    std::deque<uint32_t> inputQueue;                              // events not yet taken
    std::vector<ActivePointer> activePointers;
    uint32_t inputQueueHandle = 0, sensorManager = 0, accelerometer = 0, sensorList = 0;
    std::unordered_map<uint32_t, SensorQueue> sensorQueues;
    float accelX = 0.0f, accelY = 0.0f, accelZ = 9.80665f;
};
Plumbing& P(Process& p) { return p.service<Plumbing>(); }
int64_t nowNs() { return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count(); }

// Whether a source has something now. Caller holds mu.
bool ready(Plumbing& s, const Source& src) {
    if (src.object == s.inputQueueHandle && src.object) return !s.inputQueue.empty();
    auto it = s.sensorQueues.find(src.object);
    if (it == s.sensorQueues.end()) return false;
    const SensorQueue& q = it->second;
    return q.accelEnabled && clock::now() - q.last >= std::chrono::nanoseconds(q.periodNs);
}

// ----------------------------------------------------------------- ALooper ---
uint32_t l_forThread(Ctx& x) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.threadLooper.find(std::this_thread::get_id());
    return it == s.threadLooper.end() ? 0 : it->second;
}
uint32_t l_prepare(Ctx& x, int32_t) {
    if (const uint32_t l = l_forThread(x)) return l;
    const uint32_t l = x.proc.heap().allocZeroed(16);
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    s.loopers[l];
    s.threadLooper[std::this_thread::get_id()] = l;
    return l;
}
void l_acquire(Ctx&, GuestPtr) {}
void l_release(Ctx&, GuestPtr) {}
void l_wake(Ctx& x, GuestPtr l) {
    auto& s = P(x.proc);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (auto it = s.loopers.find(l); it != s.loopers.end()) it->second.woken = true;
    }
    s.cv.notify_all();
}
int32_t l_addFd(Ctx& x, GuestPtr l, int32_t fd, int32_t ident, int32_t, GuestPtr callback, GuestPtr data) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.loopers.find(l);
    if (it == s.loopers.end()) return -1;
    it->second.sources.push_back({fd, ident, callback, data, 0});
    x.proc.reportDefaulted("ALooper_addFd: descriptor " + std::to_string(fd) + " is never reported ready");
    return 1;
}
int32_t l_removeFd(Ctx& x, GuestPtr l, int32_t fd) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.loopers.find(l);
    if (it == s.loopers.end()) return 0;
    auto& v = it->second.sources;
    const size_t before = v.size();
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Source& src) { return src.fd == fd && !src.object; }), v.end());
    return before != v.size() ? 1 : 0;
}
// pollAll(timeoutMillis, outFd, outEvents, outData): an ident with events, or WAKE / TIMEOUT.
// Callback sources are called here (and polling goes on, as pollAll does).
int32_t l_pollAll(Ctx& x, int32_t timeoutMs, GuestPtr outFd, GuestPtr outEvents, GuestPtr outData) {
    auto& s = P(x.proc);
    const uint32_t l = l_forThread(x);
    const auto due = timeoutMs < 0 ? clock::time_point::max() : clock::now() + std::chrono::milliseconds(timeoutMs);
    std::unique_lock<std::mutex> lk(s.mu);
    auto it = s.loopers.find(l);
    if (it == s.loopers.end()) {
        lk.unlock();
        if (timeoutMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeoutMs, 50)));
        return kPollTimeout;
    }
    BlockedScope blocked(x.proc, "ALooper_pollAll");
    for (;;) {
        Looper& lp = s.loopers[l];
        if (lp.woken) { lp.woken = false; return kPollWake; }
        bool calledBack = false;
        for (size_t i = 0; i < lp.sources.size(); ++i) {
            const Source src = lp.sources[i];
            if (!ready(s, src)) continue;
            if (src.callback) {
                lk.unlock();
                const int32_t keep = int32_t(x.proc.callGuest(x.cpu, src.callback, {uint32_t(src.fd), 1u, src.data}));
                lk.lock();
                if (!keep) {
                    auto& v = s.loopers[l].sources;
                    v.erase(std::remove_if(v.begin(), v.end(), [&](const Source& o) { return o.object == src.object && o.fd == src.fd; }), v.end());
                }
                calledBack = true;
                break;
            }
            if (outFd) x.mem.write<int32_t>(outFd, src.fd);
            if (outEvents) x.mem.write<int32_t>(outEvents, 1);   // ALOOPER_EVENT_INPUT
            if (outData) x.mem.write<uint32_t>(outData, src.data);
            return src.ident;
        }
        if (calledBack) continue;
        if (clock::now() >= due) return kPollTimeout;
        if (x.proc.stopping()) throw ProcessExit(0, "stopping");
        if (x.cpu.async_pending.load(std::memory_order_acquire)) {
            lk.unlock();
            x.proc.engine().runSafepoint(x.cpu);
            lk.lock();
        }
        auto wait = std::chrono::milliseconds(5);
        if (due != clock::time_point::max()) wait = std::min(wait, std::chrono::duration_cast<std::chrono::milliseconds>(due - clock::now()) + std::chrono::milliseconds(1));
        s.cv.wait_for(lk, wait);
    }
}
int32_t l_pollOnce(Ctx& x, int32_t timeoutMs, GuestPtr outFd, GuestPtr outEvents, GuestPtr outData) {
    return l_pollAll(x, timeoutMs, outFd, outEvents, outData);
}

// ------------------------------------------------------------- input queue ---
void q_attach(Ctx& x, GuestPtr q, GuestPtr looper, int32_t ident, GuestPtr callback, GuestPtr data) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    s.loopers[looper].sources.push_back({-1, ident, callback, data, q});
}
void q_detach(Ctx& x, GuestPtr q) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    for (auto& [h, lp] : s.loopers)
        lp.sources.erase(std::remove_if(lp.sources.begin(), lp.sources.end(), [&](const Source& o) { return o.object == q; }), lp.sources.end());
}
int32_t q_hasEvents(Ctx& x, GuestPtr) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    return s.inputQueue.empty() ? 0 : 1;
}
int32_t q_getEvent(Ctx& x, GuestPtr, GuestPtr out) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    if (s.inputQueue.empty()) return -1;
    x.mem.write<uint32_t>(out, s.inputQueue.front());
    s.inputQueue.pop_front();
    return 0;
}
int32_t q_preDispatch(Ctx&, GuestPtr, GuestPtr) { return 0; }   // no IME
void q_finish(Ctx& x, GuestPtr, GuestPtr ev, int32_t) {
    auto& s = P(x.proc);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        if (!s.events.erase(ev)) return;
    }
    x.proc.heap().free(ev);
}

// ------------------------------------------------------------ input events ---
template <typename F> auto withEvent(Ctx& x, GuestPtr ev, F f) -> decltype(f(std::declval<const Event&>())) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.events.find(ev);
    static const Event none;
    return f(it == s.events.end() ? none : it->second);
}
int32_t e_type(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.type; }); }
int32_t e_source(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.source; }); }
int32_t e_deviceId(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.deviceId; }); }
int32_t e_action(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.action; }); }
int32_t e_zero(Ctx&, GuestPtr) { return 0; }
int32_t e_meta(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.metaState; }); }
int32_t e_keyCode(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.keyCode; }); }
int64_t e_downTime(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.downTime; }); }
int64_t e_eventTime(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return e.eventTime; }); }
uint32_t e_pointerCount(Ctx& x, GuestPtr ev) { return withEvent(x, ev, [](const Event& e) { return uint32_t(e.ids.size()); }); }
int32_t e_pointerId(Ctx& x, GuestPtr ev, uint32_t i) {
    return withEvent(x, ev, [i](const Event& e) { return i < e.ids.size() ? e.ids[i] : 0; });
}
float e_x(Ctx& x, GuestPtr ev, uint32_t i) { return withEvent(x, ev, [i](const Event& e) { return i < e.xs.size() ? e.xs[i] : 0.0f; }); }
float e_y(Ctx& x, GuestPtr ev, uint32_t i) { return withEvent(x, ev, [i](const Event& e) { return i < e.ys.size() ? e.ys[i] : 0.0f; }); }
float e_one(Ctx&, GuestPtr, uint32_t) { return 1.0f; }
float e_touchSize(Ctx&, GuestPtr, uint32_t) { return 20.0f; }
float e_zeroF(Ctx&, GuestPtr, uint32_t) { return 0.0f; }
float e_precision(Ctx&, GuestPtr) { return 1.0f; }
float e_zeroF1(Ctx&, GuestPtr) { return 0.0f; }
float e_axis(Ctx& x, GuestPtr ev, int32_t axis, uint32_t i) {
    return withEvent(x, ev, [axis, i](const Event& e) {
        if (e.source == 0x1000010) {   // AINPUT_SOURCE_JOYSTICK
            switch (axis) {
                case 0: return e.joyX;      // AMOTION_EVENT_AXIS_X
                case 1: return e.joyY;      // AMOTION_EVENT_AXIS_Y
                case 11: return e.joyZ;     // AMOTION_EVENT_AXIS_Z
                case 14: return e.joyRz;    // AMOTION_EVENT_AXIS_RZ
                case 15: return e.joyHatX;  // AMOTION_EVENT_AXIS_HAT_X
                case 16: return e.joyHatY;  // AMOTION_EVENT_AXIS_HAT_Y
                default: return 0.0f;
            }
        }
        return axis == 0 ? (i < e.xs.size() ? e.xs[i] : 0.0f) : axis == 1 ? (i < e.ys.size() ? e.ys[i] : 0.0f) : 0.0f;
    });
}
uint32_t e_historySize(Ctx&, GuestPtr) { return 0; }
int64_t e_historicalTime(Ctx& x, GuestPtr ev, uint32_t) { return e_eventTime(x, ev); }
float e_historical(Ctx& x, GuestPtr ev, uint32_t i, uint32_t) { return e_x(x, ev, i); }
float e_historicalY(Ctx& x, GuestPtr ev, uint32_t i, uint32_t) { return e_y(x, ev, i); }
float e_historicalOne(Ctx&, GuestPtr, uint32_t, uint32_t) { return 1.0f; }
int32_t e_toolType(Ctx&, GuestPtr, uint32_t) { return 1; }   // finger

// ----------------------------------------------------------------- sensors ---
// ASensor (an opaque pointer to these): type, name, vendor, resolution, minDelay.
uint32_t s_instance(Ctx& x) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.sensorManager) {
        s.sensorManager = x.proc.allocStatic(16);
        s.accelerometer = x.proc.allocStatic(16);
        x.mem.write<int32_t>(s.accelerometer, 1);   // ASENSOR_TYPE_ACCELEROMETER
        s.sensorList = x.proc.allocStatic(8);
        x.mem.write<uint32_t>(s.sensorList, s.accelerometer);
    }
    return s.sensorManager;
}
uint32_t s_instanceForPackage(Ctx& x, GuestPtr) { return s_instance(x); }
int32_t s_list(Ctx& x, GuestPtr, GuestPtr out) {
    s_instance(x);
    if (out) x.mem.write<uint32_t>(out, P(x.proc).sensorList);
    return 1;
}
uint32_t s_default(Ctx& x, GuestPtr, int32_t type) {
    s_instance(x);
    return type == 1 ? P(x.proc).accelerometer : 0;
}
uint32_t s_createQueue(Ctx& x, GuestPtr, GuestPtr looper, int32_t ident, GuestPtr callback, GuestPtr data) {
    const uint32_t q = x.proc.heap().allocZeroed(16);
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    s.sensorQueues[q].looper = looper;
    s.loopers[looper].sources.push_back({-1, ident, callback, data, q});
    return q;
}
int32_t s_destroyQueue(Ctx& x, GuestPtr, GuestPtr q) {
    auto& s = P(x.proc);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.sensorQueues.erase(q);
        for (auto& [h, lp] : s.loopers)
            lp.sources.erase(std::remove_if(lp.sources.begin(), lp.sources.end(), [&](const Source& o) { return o.object == q; }), lp.sources.end());
    }
    x.proc.heap().free(q);
    return 0;
}
int32_t s_enable(Ctx& x, GuestPtr q, GuestPtr sensor) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.sensorQueues.find(q);
    if (it == s.sensorQueues.end() || sensor != s.accelerometer) return -22;
    it->second.accelEnabled = true;
    return 0;
}
int32_t s_disable(Ctx& x, GuestPtr q, GuestPtr sensor) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.sensorQueues.find(q);
    if (it == s.sensorQueues.end() || sensor != s.accelerometer) return -22;
    it->second.accelEnabled = false;
    return 0;
}
int32_t s_setRate(Ctx& x, GuestPtr q, GuestPtr, int32_t usec) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.sensorQueues.find(q);
    if (it == s.sensorQueues.end()) return -22;
    it->second.periodNs = std::max<int64_t>(int64_t(usec) * 1000, 10'000'000);   // at most 100 Hz
    return 0;
}
int32_t s_hasEvents(Ctx& x, GuestPtr q) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    Source src;
    src.object = q;
    return ready(s, src) ? 1 : 0;
}
// ASensorEvent (104 bytes): version, sensor, type, reserved, int64 timestamp, float data[16], ...
int32_t s_getEvents(Ctx& x, GuestPtr q, GuestPtr out, uint32_t count) {
    auto& s = P(x.proc);
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.sensorQueues.find(q);
    Source src;
    src.object = q;
    if (it == s.sensorQueues.end() || !count || !ready(s, src)) return 0;
    it->second.last = clock::now();
    std::memset(x.mem.host(out), 0, 104);
    x.mem.write<int32_t>(out, 104);
    x.mem.write<int32_t>(out + 4, 1);
    x.mem.write<int32_t>(out + 8, 1);
    x.mem.write<int64_t>(out + 16, nowNs());
    x.mem.write<float>(out + 24, s.accelX);
    x.mem.write<float>(out + 28, s.accelY);
    x.mem.write<float>(out + 32, s.accelZ);
    return 1;
}
int32_t s_type(Ctx& x, GuestPtr sensor) { return sensor ? x.mem.read<int32_t>(sensor) : 0; }
uint32_t s_name(Ctx& x, GuestPtr) {
    static const char key = 0;
    uint32_t& str = x.proc.threadSlot(&key);
    if (!str) { str = x.proc.allocStatic(32); { const char* t = "HydrogenVM accelerometer"; std::memcpy(x.mem.host(str), t, std::strlen(t) + 1); } }
    return str;
}
uint32_t s_vendor(Ctx& x, GuestPtr) {
    static const char key = 0;
    uint32_t& str = x.proc.threadSlot(&key);
    if (!str) { str = x.proc.allocStatic(16); { const char* t = "HydrogenVM"; std::memcpy(x.mem.host(str), t, std::strlen(t) + 1); } }
    return str;
}
float s_resolution(Ctx&, GuestPtr) { return 0.01f; }
int32_t s_minDelay(Ctx&, GuestPtr) { return 10000; }

} // namespace

uint32_t inputQueue(Process& p) {
    auto& s = P(p);
    std::lock_guard<std::mutex> lk(s.mu);
    if (!s.inputQueueHandle) s.inputQueueHandle = p.allocStatic(16);
    return s.inputQueueHandle;
}

void pushTouchEvent(Process& p, TouchPhase phase, float x, float y, uint32_t id, uint32_t) {
    const uint32_t h = p.heap().allocZeroed(16);
    inputQueue(p);
    auto& s = P(p);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        auto findIdx = [&](int32_t pid) -> int {
            for (size_t i = 0; i < s.activePointers.size(); ++i)
                if (s.activePointers[i].id == pid) return int(i);
            return -1;
        };
        int idx = findIdx(int32_t(id));
        if (phase == TouchPhase::Began) {
            if (idx < 0) {
                s.activePointers.push_back({int32_t(id), x, y});
                idx = int(s.activePointers.size() - 1);
            } else {
                s.activePointers[idx].x = x;
                s.activePointers[idx].y = y;
            }
        } else if (phase == TouchPhase::Moved) {
            if (idx < 0) {
                s.activePointers.push_back({int32_t(id), x, y});
                idx = int(s.activePointers.size() - 1);
            } else {
                s.activePointers[idx].x = x;
                s.activePointers[idx].y = y;
            }
        } else {
            if (idx < 0) {
                s.activePointers.push_back({int32_t(id), x, y});
                idx = int(s.activePointers.size() - 1);
            } else {
                s.activePointers[idx].x = x;
                s.activePointers[idx].y = y;
            }
        }

        Event e;
        e.type = 2;
        e.source = 0x1002;
        e.downTime = e.eventTime = nowNs();
        switch (phase) {
            case TouchPhase::Began:
                e.action = s.activePointers.size() <= 1 ? 0 : (5 | (idx << 8));
                break;
            case TouchPhase::Moved:
                e.action = 2;
                break;
            case TouchPhase::Cancelled:
                e.action = 3;
                break;
            default:
                e.action = s.activePointers.size() <= 1 ? 1 : (6 | (idx << 8));
                break;
        }
        for (const auto& pt : s.activePointers) {
            e.ids.push_back(pt.id);
            e.xs.push_back(pt.x);
            e.ys.push_back(pt.y);
        }
        if (phase == TouchPhase::Cancelled) {
            s.activePointers.clear();
        } else if (phase == TouchPhase::Ended) {
            s.activePointers.erase(s.activePointers.begin() + idx);
        }
        s.events[h] = std::move(e);
        s.inputQueue.push_back(h);
    }
    s.cv.notify_all();
}

void pushKeyEvent(Process& p, int32_t keyCode, bool down, int32_t deviceId, int32_t source) {
    Event e;
    e.type = 1; // AINPUT_EVENT_TYPE_KEY
    e.action = down ? 0 : 1; // AKEY_EVENT_ACTION_DOWN / UP
    e.source = source;
    e.deviceId = deviceId;
    e.keyCode = keyCode;
    e.downTime = e.eventTime = nowNs();
    const uint32_t h = p.heap().allocZeroed(16);
    inputQueue(p);
    auto& s = P(p);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.events[h] = std::move(e);
        s.inputQueue.push_back(h);
    }
    s.cv.notify_all();
}

void pushJoystickEvent(Process& p, float lx, float ly, float rx, float ry, float hatX, float hatY, int32_t deviceId) {
    Event e;
    e.type = 2; // AINPUT_EVENT_TYPE_MOTION
    e.action = 2; // AMOTION_EVENT_ACTION_MOVE
    e.source = 0x1000010; // AINPUT_SOURCE_JOYSTICK
    e.deviceId = deviceId;
    e.downTime = e.eventTime = nowNs();
    e.ids = {0};
    e.xs = {lx};
    e.ys = {ly};
    e.joyX = lx;
    e.joyY = ly;
    e.joyZ = rx;
    e.joyRz = ry;
    e.joyHatX = hatX;
    e.joyHatY = hatY;
    const uint32_t h = p.heap().allocZeroed(16);
    inputQueue(p);
    auto& s = P(p);
    {
        std::lock_guard<std::mutex> lk(s.mu);
        s.events[h] = std::move(e);
        s.inputQueue.push_back(h);
    }
    s.cv.notify_all();
}

void setAndroidAcceleration(Process& p, float gx, float gy, float gz) {
    constexpr float kG = 9.80665f;
    auto& s = P(p);
    std::lock_guard<std::mutex> lk(s.mu);
    // CoreMotion UIAcceleration is in g (screen up => z = -1g); Android sensor is in m/s^2 (screen up => +9.81).
    s.accelX = -gx * kG;
    s.accelY = -gy * kG;
    s.accelZ = -gz * kG;
}

void registerLooper(Process& p) {
#define R(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Real, WHAT)
#define A(F, NAME, WHAT) supportDef<&F>(p, std::string("android:") + NAME, SupportStatus::Approx, WHAT)
    R(l_prepare, "ALooper_prepare", "the thread's looper");
    R(l_forThread, "ALooper_forThread", "the thread's looper");
    R(l_acquire, "ALooper_acquire", "loopers live as long as the game");
    R(l_release, "ALooper_release", "loopers live as long as the game");
    R(l_wake, "ALooper_wake", "ends a poll");
    A(l_addFd, "ALooper_addFd", "kept; plain descriptors are never reported ready");
    R(l_removeFd, "ALooper_removeFd", "drops it");
    R(l_pollAll, "ALooper_pollAll", "input and sensor queues, wake, timeout");
    R(l_pollOnce, "ALooper_pollOnce", "as pollAll");
    R(q_attach, "AInputQueue_attachLooper", "touches go to the looper");
    R(q_detach, "AInputQueue_detachLooper", "detached");
    R(q_hasEvents, "AInputQueue_hasEvents", "touch events waiting");
    R(q_getEvent, "AInputQueue_getEvent", "next touch event");
    R(q_preDispatch, "AInputQueue_preDispatchEvent", "no input method");
    R(q_finish, "AInputQueue_finishEvent", "frees the event");
    R(e_type, "AInputEvent_getType", "motion or key");
    R(e_source, "AInputEvent_getSource", "touchscreen");
    R(e_deviceId, "AInputEvent_getDeviceId", "one touchscreen");
    R(e_action, "AMotionEvent_getAction", "DOWN / MOVE / UP / POINTER_*");
    R(e_zero, "AMotionEvent_getFlags", "0");
    R(e_meta, "AMotionEvent_getMetaState", "0");
    R(e_zero, "AMotionEvent_getEdgeFlags", "0");
    R(e_zero, "AMotionEvent_getButtonState", "0");
    R(e_downTime, "AMotionEvent_getDownTime", "ns");
    R(e_eventTime, "AMotionEvent_getEventTime", "ns");
    R(e_precision, "AMotionEvent_getXPrecision", "1");
    R(e_precision, "AMotionEvent_getYPrecision", "1");
    R(e_zeroF1, "AMotionEvent_getXOffset", "0");
    R(e_zeroF1, "AMotionEvent_getYOffset", "0");
    R(e_pointerCount, "AMotionEvent_getPointerCount", "all active pointers");
    R(e_pointerId, "AMotionEvent_getPointerId", "the finger");
    R(e_toolType, "AMotionEvent_getToolType", "finger");
    R(e_x, "AMotionEvent_getX", "surface pixels");
    R(e_y, "AMotionEvent_getY", "surface pixels");
    R(e_x, "AMotionEvent_getRawX", "surface pixels");
    R(e_y, "AMotionEvent_getRawY", "surface pixels");
    R(e_one, "AMotionEvent_getPressure", "1");
    R(e_one, "AMotionEvent_getSize", "1");
    R(e_touchSize, "AMotionEvent_getTouchMajor", "20 px");
    R(e_touchSize, "AMotionEvent_getTouchMinor", "20 px");
    R(e_touchSize, "AMotionEvent_getToolMajor", "20 px");
    R(e_touchSize, "AMotionEvent_getToolMinor", "20 px");
    R(e_zeroF, "AMotionEvent_getOrientation", "0");
    R(e_axis, "AMotionEvent_getAxisValue", "x, y, and joystick axes");
    R(e_historySize, "AMotionEvent_getHistorySize", "no history");
    R(e_historicalTime, "AMotionEvent_getHistoricalEventTime", "event time");
    R(e_historical, "AMotionEvent_getHistoricalX", "current x");
    R(e_historicalY, "AMotionEvent_getHistoricalY", "current y");
    R(e_historicalOne, "AMotionEvent_getHistoricalPressure", "1");
    R(e_historicalOne, "AMotionEvent_getHistoricalSize", "1");
    R(e_action, "AKeyEvent_getAction", "key action");
    R(e_keyCode, "AKeyEvent_getKeyCode", "key code");
    R(e_meta, "AKeyEvent_getMetaState", "0");
    R(e_zero, "AKeyEvent_getFlags", "0");
    R(e_zero, "AKeyEvent_getRepeatCount", "0");
    R(e_zero, "AKeyEvent_getScanCode", "0");
    R(e_downTime, "AKeyEvent_getDownTime", "ns");
    R(e_eventTime, "AKeyEvent_getEventTime", "ns");
    R(s_instance, "ASensorManager_getInstance", "one accelerometer");
    R(s_instanceForPackage, "ASensorManager_getInstanceForPackage", "one accelerometer");
    R(s_list, "ASensorManager_getSensorList", "one accelerometer");
    R(s_default, "ASensorManager_getDefaultSensor", "the accelerometer");
    R(s_createQueue, "ASensorManager_createEventQueue", "on the looper");
    R(s_destroyQueue, "ASensorManager_destroyEventQueue", "frees it");
    R(s_enable, "ASensorEventQueue_enableSensor", "accelerometer events");
    R(s_disable, "ASensorEventQueue_disableSensor", "stops them");
    R(s_setRate, "ASensorEventQueue_setEventRate", "at most 100 Hz");
    R(s_hasEvents, "ASensorEventQueue_hasEvents", "one per period");
    R(s_getEvents, "ASensorEventQueue_getEvents", "accelerometer events from host");
    R(s_type, "ASensor_getType", "accelerometer");
    R(s_name, "ASensor_getName", "fixed");
    R(s_vendor, "ASensor_getVendor", "fixed");
    R(s_resolution, "ASensor_getResolution", "0.01");
    R(s_minDelay, "ASensor_getMinDelay", "10 ms");
#undef R
#undef A
}

} // namespace Hydrogen::Runtime::Android
