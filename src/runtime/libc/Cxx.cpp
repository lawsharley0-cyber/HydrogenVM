// HydrogenVM — the parts of the C++ runtime (libstdc++ / libc++abi) that
// apps import directly: operator new/delete, guard variables, atexit,
// exception entry points, SJLJ unwinder registration.
//
// Exceptions and the SJLJ unwinder live in Unwind.cpp.
#include "runtime/Process.hpp"
#include <cxxabi.h>

namespace Hydrogen::Runtime {
namespace {

uint32_t x_new(Ctx& x, uint32_t n) {
    const uint32_t a = x.proc.heap().alloc(n);
    if (!a) throw std::runtime_error("operator new: out of guest memory");
    return a;
}
uint32_t x_new_nothrow(Ctx& x, uint32_t n, GuestPtr) { return x.proc.heap().alloc(n); }
void x_delete(Ctx& x, GuestPtr p) { x.proc.heap().free(p); }
void x_delete2(Ctx& x, GuestPtr p, GuestPtr) { x.proc.heap().free(p); }

// Guard variables: byte 0 set = initialised. We serialise with one lock;
// recursion from the same thread is a program error anyway.
struct Guards { explicit Guards(Process&) {} std::recursive_mutex mu; };
int32_t x_guard_acquire(Ctx& x, GuestPtr g) {
    if (x.mem.read<uint8_t>(g) & 1) return 0;
    x.proc.service<Guards>().mu.lock();
    if (x.mem.read<uint8_t>(g) & 1) { x.proc.service<Guards>().mu.unlock(); return 0; }
    return 1;
}
void x_guard_release(Ctx& x, GuestPtr g) { x.mem.write<uint8_t>(g, 1); x.proc.service<Guards>().mu.unlock(); }
void x_guard_abort(Ctx& x, GuestPtr) { x.proc.service<Guards>().mu.unlock(); }


void x_void1(Ctx&, GuestPtr) {}
void x_terminate(Ctx&) { throw std::runtime_error("std::terminate() called"); }
void x_pure_virtual(Ctx&) { throw std::runtime_error("pure virtual function called"); }
void x_throw_msg(Ctx& x, GuestPtr msg) {
    throw std::runtime_error("C++ library error: " + (msg ? x.proc.readCString(msg) : std::string("(no message)")));
}
uint32_t x_demangle(Ctx& x, GuestPtr mangled, GuestPtr buf, GuestPtr lenp, GuestPtr statusp) {
    int st = 0;
    char* d = abi::__cxa_demangle(x.proc.readCString(mangled).c_str(), nullptr, nullptr, &st);
    if (statusp) x.mem.write<int32_t>(statusp, st);
    if (!d || st) { std::free(d); return 0; }
    const std::string s = d;
    std::free(d);
    if (buf) x.proc.heap().free(buf);
    const uint32_t out = x.proc.allocCString(s);
    if (lenp) x.mem.write<uint32_t>(lenp, uint32_t(s.size() + 1));
    return out;
}

} // namespace

void registerCxxRuntime(Process& p) {
    for (const char* n : {"__Znwj", "__Znaj", "__Znwm", "__Znam"}) p.def<&x_new>(n);
    for (const char* n : {"__ZnwjRKSt9nothrow_t", "__ZnajRKSt9nothrow_t", "__ZnwmRKSt9nothrow_t", "__ZnamRKSt9nothrow_t"}) p.def<&x_new_nothrow>(n);
    for (const char* n : {"__ZdlPv", "__ZdaPv"}) p.def<&x_delete>(n);
    for (const char* n : {"__ZdlPvRKSt9nothrow_t", "__ZdaPvRKSt9nothrow_t", "__ZdlPvj", "__ZdaPvj"}) p.def<&x_delete2>(n);
    p.defineDataBlock("__ZSt7nothrow", 4);
    p.defineDataBlock("___dso_handle", 4);
    p.def<&x_guard_acquire>("___cxa_guard_acquire");
    p.def<&x_guard_release>("___cxa_guard_release");
    p.def<&x_guard_abort>("___cxa_guard_abort");
    p.def<&x_void1>("___cxa_finalize");
    p.def<&x_terminate>("__ZSt9terminatev");
    p.def<&x_terminate>("___cxa_call_unexpected");
    p.def<&x_pure_virtual>("___cxa_pure_virtual");
    p.def<&x_pure_virtual>("___cxa_deleted_virtual");
    p.def<&x_demangle>("___cxa_demangle");
    for (const char* n : {"__ZSt20__throw_length_errorPKc", "__ZSt19__throw_logic_errorPKc", "__ZSt20__throw_out_of_rangePKc",
                          "__ZSt24__throw_out_of_range_fmtPKcz", "__ZSt24__throw_invalid_argumentPKc", "__ZSt21__throw_runtime_errorPKc",
                          "__ZSt17__throw_bad_allocv", "__ZSt16__throw_bad_castv", "__ZSt19__throw_range_errorPKc",
                          "__ZSt21__throw_bad_exceptionv", "__ZSt25__throw_bad_function_callv"})
        p.def<&x_throw_msg>(n);
}

} // namespace Hydrogen::Runtime
