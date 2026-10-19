// HydrogenVM — guest calling convention helpers (iOS armv7 ABI).
//
// The iOS 32-bit ABI is AAPCS-like with Apple's differences:
//  * floats and doubles are passed and returned in core registers (softfp);
//  * 64-bit arguments are NOT aligned to an even register or 8-byte stack
//    slot — they just take the next two words;
//  * structs larger than 4 bytes are returned through a hidden pointer in r0.
//
// Host functions are written as ordinary C++ functions taking a Ctx& first:
//
//     uint32_t hle_strlen(Ctx& x, GuestPtr s);
//     double   hle_atan2(Ctx& x, double y, double x2);
//     int32_t  hle_printf(Ctx& x, GuestPtr fmt, VarArgs va);
//
// and registered with `wrap<&hle_strlen>()`, which generates the code that
// pulls arguments out of r0-r3 and the stack and puts the result back.
#pragma once
#include "core/cpu/Cpu.hpp"
#include "core/cpu/Engine.hpp"
#include "core/memory/GuestArena.hpp"
#include <cstring>
#include <tuple>
#include <type_traits>

namespace Hydrogen::Runtime {

class Process;

using GuestPtr = uint32_t;

// Context handed to every host function.
struct Ctx {
    Process& proc;
    CPU::Cpu& cpu;
    Memory::GuestArena& mem;
};

// Sequential reader over a guest argument list: either the call's own
// registers + stack (starting at a word index), or a va_list in memory.
class ArgReader {
public:
    ArgReader(CPU::Cpu& c, Memory::GuestArena& m, uint32_t slot) : c_(&c), m_(&m), slot_(slot), mem_(0) {}
    ArgReader(Memory::GuestArena& m, GuestPtr vaList) : c_(nullptr), m_(&m), slot_(0), mem_(vaList) {}

    uint32_t word() {
        if (c_) {
            const uint32_t i = slot_++;
            return i < 4 ? c_->r[i] : m_->read<uint32_t>(c_->r[CPU::kSP] + (i - 4) * 4);
        }
        const uint32_t v = m_->read<uint32_t>(mem_);
        mem_ += 4;
        return v;
    }
    uint64_t dword() { const uint64_t lo = word(); return lo | (uint64_t(word()) << 32); }
    template <typename T> T get() {
        if constexpr (std::is_same_v<T, double>) { const uint64_t b = dword(); double d; std::memcpy(&d, &b, 8); return d; }
        else if constexpr (std::is_same_v<T, float>) { const uint32_t b = word(); float f; std::memcpy(&f, &b, 4); return f; }
        else if constexpr (sizeof(T) == 8) return T(dword());
        else if constexpr (std::is_same_v<T, bool>) return (word() & 0xFF) != 0;
        else return T(word());
    }
    // Current position as a guest va_list pointer (only valid for in-memory lists).
    uint32_t slot() const { return slot_; }
    bool fromRegisters() const { return c_ != nullptr; }

private:
    CPU::Cpu* c_;
    Memory::GuestArena* m_;
    uint32_t slot_;
    uint32_t mem_;
};

// Marker parameter type: the remaining (variadic) arguments of the call.
struct VarArgs { ArgReader args; };
// Marker parameter type: a guest va_list (pointer to the argument area).
struct VaList { GuestPtr ptr; };
// Marker parameter type: a by-value struct argument of N words.
template <size_t N> struct Words { uint32_t w[N]; };

namespace detail {

template <typename T> struct IsWords : std::false_type {};
template <size_t N> struct IsWords<Words<N>> : std::true_type {};

template <typename T> T readArg(ArgReader& a) {
    if constexpr (std::is_same_v<T, VaList>) return VaList{a.word()};
    else if constexpr (std::is_same_v<T, VarArgs>) return VarArgs{a};
    else if constexpr (std::is_enum_v<T>) return T(a.word());
    else if constexpr (IsWords<T>::value) { T v; for (auto& x : v.w) x = a.word(); return v; }
    else return a.get<T>();
}

inline void setReturn(CPU::Cpu& c, uint64_t v) { c.r[0] = uint32_t(v); c.r[1] = uint32_t(v >> 32); }

template <typename R> void writeRet(CPU::Cpu& c, R v) {
    if constexpr (std::is_same_v<R, double>) { uint64_t b; std::memcpy(&b, &v, 8); setReturn(c, b); }
    else if constexpr (std::is_same_v<R, float>) { uint32_t b; std::memcpy(&b, &v, 4); c.r[0] = b; }
    else if constexpr (std::is_same_v<R, bool>) c.r[0] = v ? 1 : 0;
    else if constexpr (sizeof(R) == 8) setReturn(c, uint64_t(v));
    else c.r[0] = uint32_t(v);
}

template <typename F> struct Traits;
template <typename R, typename... A> struct Traits<R (*)(Ctx&, A...)> {
    using Ret = R;
    using Args = std::tuple<A...>;
};

template <auto F, typename R, typename... A, size_t... I>
void invoke(Ctx& x, std::index_sequence<I...>) {
    ArgReader rd(x.cpu, x.mem, 0);
    // Braced init guarantees left-to-right evaluation (argument order matters).
    std::tuple<std::decay_t<A>...> args{readArg<std::decay_t<A>>(rd)...};
    if constexpr (std::is_void_v<R>) F(x, std::get<I>(args)...);
    else writeRet<R>(x.cpu, F(x, std::get<I>(args)...));
}

template <auto F, typename R, typename... A>
void call(Ctx& x, std::tuple<A...>*) { invoke<F, R, A...>(x, std::index_sequence_for<A...>{}); }

} // namespace detail

// Adapter from a typed host function to an engine HLE handler body.
template <auto F>
void callWrapped(Ctx& x) {
    using T = detail::Traits<decltype(F)>;
    detail::call<F, typename T::Ret>(x, static_cast<typename T::Args*>(nullptr));
}

} // namespace Hydrogen::Runtime
