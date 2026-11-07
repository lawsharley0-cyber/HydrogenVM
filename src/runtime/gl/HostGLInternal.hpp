// HydrogenVM — HostGL internals shared by HostGL.cpp and Gles1Emu.cpp.
#pragma once
#include "runtime/gl/HostGL.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>

namespace Hydrogen::Runtime::HostGL {

struct Gles1Emu;

// The forwarded entry points, loaded per host context (the same name can resolve
// differently on an ES context and on a desktop compatibility context).
struct Fns {
#define HOSTGL_FN(name, ret, params) ret (*name) params = nullptr;
#define HOSTGL_TABLE
#include "runtime/gl/HostGLTable.inc"
#undef HOSTGL_TABLE
#undef HOSTGL_FN
    // Desktop-only replacements for ES 1.1 entry points.
    void (*glOrtho)(double, double, double, double, double, double) = nullptr;
    void (*glFrustum)(double, double, double, double, double, double) = nullptr;
    void (*glClipPlane)(uint32_t, const double*) = nullptr;
    void (*glGetClipPlane)(uint32_t, double*) = nullptr;
    // ES 3.0 / desktop names used to emulate Apple extensions.
    void* (*glFenceSync)(uint32_t, uint32_t) = nullptr;
    uint8_t (*glIsSync)(void*) = nullptr;
    void (*glDeleteSync)(void*) = nullptr;
    uint32_t (*glClientWaitSync)(void*, uint32_t, uint64_t) = nullptr;
    void (*glWaitSync)(void*, uint32_t, uint64_t) = nullptr;
    void (*glGetSynciv)(void*, uint32_t, int32_t, int32_t*, int32_t*) = nullptr;
};

struct MappedBuffer {
    uint32_t guest = 0;      // guest shadow copy handed to the app
    void* host = nullptr;    // the driver's mapping
    intptr_t offset = 0, length = 0;
    uint32_t access = 0;
};

struct HostContext {
    void* handle = nullptr;
    int api = 2;
    bool desktop = false;    // ES 1.1 guest on a desktop compatibility context
    bool implicitWorker = false;
    uint64_t retireId = 0;               // unique per retirement (set under State::mu); never reused, unlike addresses
    std::atomic<bool> retired{false};   // its sharegroup was destroyed; the owning thread must drop it
    uint32_t guestContext = 0, guestSharegroup = 0;
    uint32_t boundFbo = 0;
    uint32_t currentProgram = 0;
    Fns fn;
    uint32_t readFbo = 0;    // scratch FBO for read-backs
    // A guest error that was pending when the runtime made GL calls of its own
    // in this context (see GuestErrorScope); glGetError reports it first.
    uint32_t guestError = 0;
    bool trace = false;           // ProcessConfig::glTrace when the context was made
    std::unordered_map<uint32_t, MappedBuffer> mapped;   // key: target
    // ES 1.1 guest on an ES 2/3 host context: fixed function emulated in shaders.
    std::shared_ptr<Gles1Emu> gles1;
};

// Brackets GL calls the runtime makes in a guest's context (frame sampling,
// read-back, worker-context sync). The guest's pending error is kept for its
// next glGetError, and errors the runtime's own calls raise are discarded, so
// they never reach a game that checks glGetError after its own calls (Unity
// and NaturalMotion builds trap on any error).
struct GuestErrorScope {
    HostContext* hc;
    explicit GuestErrorScope(HostContext* c) : hc(c) {
        if (!hc || !hc->fn.glGetError) return;
        const uint32_t e = hc->fn.glGetError();
        if (e && !hc->guestError) hc->guestError = e;
    }
    ~GuestErrorScope() {
        if (!hc || !hc->fn.glGetError) return;
        for (int i = 0; i < 16 && hc->fn.glGetError(); ++i) {}
    }
    GuestErrorScope(const GuestErrorScope&) = delete;
    GuestErrorScope& operator=(const GuestErrorScope&) = delete;
};

// The host context current on the calling thread (null if none).
HostContext* currentHostContext();

// Gles1Emu.cpp: install the fixed-function emulation into hc's function table
// (after the table was loaded from the ES 2/3 driver).
void installGles1Emulation(HostContext& hc);


} // namespace Hydrogen::Runtime::HostGL
