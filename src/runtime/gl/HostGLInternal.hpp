// HydrogenVM — HostGL internals shared by HostGL.cpp and Gles1Emu.cpp.
#pragma once
#include "runtime/gl/HostGL.hpp"
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
    uint32_t guestContext = 0, guestSharegroup = 0;
    Fns fn;
    uint32_t readFbo = 0;    // scratch FBO for read-backs
    std::unordered_map<uint32_t, MappedBuffer> mapped;   // key: target
    // ES 1.1 guest on an ES 2/3 host context: fixed function emulated in shaders.
    std::shared_ptr<Gles1Emu> gles1;
};

// The host context current on the calling thread (null if none).
HostContext* currentHostContext();

// Gles1Emu.cpp: install the fixed-function emulation into hc's function table
// (after the table was loaded from the ES 2/3 driver).
void installGles1Emulation(HostContext& hc);


} // namespace Hydrogen::Runtime::HostGL
