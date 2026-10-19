// HydrogenVM — compiler runtime helpers that iOS apps import from libSystem
// (libcompiler_rt): integer division (no hardware divide before the A6),
// 64-bit arithmetic, bit counting, int/float conversion, and the `*vfp`
// soft-float entry points that armv6 Thumb code calls for every float op.
//
// Semantics follow ARMv7 hardware where the C standard leaves things
// undefined: x/0 == 0, INT_MIN/-1 == INT_MIN, float→int conversion saturates
// and NaN → 0 (like VCVT).
#include "runtime/Process.hpp"
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace Hydrogen::Runtime {
namespace {

template <typename I, typename F> I satConv(F f) {
    if (std::isnan(f)) return 0;
    if (f <= F(std::numeric_limits<I>::min())) return std::numeric_limits<I>::min();
    if (f >= F(std::numeric_limits<I>::max())) return std::numeric_limits<I>::max();
    return I(f);
}

// ---- 32-bit division
uint32_t udivsi3(Ctx&, uint32_t a, uint32_t b) { return b ? a / b : 0; }
uint32_t umodsi3(Ctx&, uint32_t a, uint32_t b) { return b ? a % b : a; }
int32_t divsi3(Ctx&, int32_t a, int32_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int32_t>::min() && b == -1) return a;
    return a / b;
}
int32_t modsi3(Ctx&, int32_t a, int32_t b) {
    if (!b) return a;
    if (b == -1) return 0;
    return a % b;
}
uint32_t udivmodsi4(Ctx& x, uint32_t a, uint32_t b, GuestPtr rem) {
    if (rem) x.mem.write<uint32_t>(rem, b ? a % b : a);
    return b ? a / b : 0;
}
int32_t divmodsi4(Ctx& x, int32_t a, int32_t b, GuestPtr rem) {
    const int32_t q = divsi3(x, a, b);
    if (rem) x.mem.write<int32_t>(rem, int32_t(uint32_t(a) - uint32_t(q) * uint32_t(b)));
    return q;
}

// ---- 64-bit division and arithmetic
uint64_t udivdi3(Ctx&, uint64_t a, uint64_t b) { return b ? a / b : 0; }
uint64_t umoddi3(Ctx&, uint64_t a, uint64_t b) { return b ? a % b : a; }
int64_t divdi3(Ctx&, int64_t a, int64_t b) {
    if (!b) return 0;
    if (a == std::numeric_limits<int64_t>::min() && b == -1) return a;
    return a / b;
}
int64_t moddi3(Ctx&, int64_t a, int64_t b) {
    if (!b) return a;
    if (b == -1) return 0;
    return a % b;
}
uint64_t udivmoddi4(Ctx& x, uint64_t a, uint64_t b, GuestPtr rem) {
    if (rem) x.mem.write<uint64_t>(rem, b ? a % b : a);
    return b ? a / b : 0;
}
int64_t divmoddi4(Ctx& x, int64_t a, int64_t b, GuestPtr rem) {
    const int64_t q = divdi3(x, a, b);
    if (rem) x.mem.write<int64_t>(rem, int64_t(uint64_t(a) - uint64_t(q) * uint64_t(b)));
    return q;
}
uint64_t muldi3(Ctx&, uint64_t a, uint64_t b) { return a * b; }
int64_t negdi2(Ctx&, int64_t a) { return int64_t(0 - uint64_t(a)); }
uint64_t ashldi3(Ctx&, uint64_t a, int32_t b) { return (b & 63) ? a << (b & 63) : a; }
uint64_t lshrdi3(Ctx&, uint64_t a, int32_t b) { return (b & 63) ? a >> (b & 63) : a; }
int64_t ashrdi3(Ctx&, int64_t a, int32_t b) { return (b & 63) ? a >> (b & 63) : a; }
// __cmpdi2 / __ucmpdi2 return 0, 1, 2 for <, ==, >.
int32_t cmpdi2(Ctx&, int64_t a, int64_t b) { return a < b ? 0 : a == b ? 1 : 2; }
int32_t ucmpdi2(Ctx&, uint64_t a, uint64_t b) { return a < b ? 0 : a == b ? 1 : 2; }
int64_t absvdi2(Ctx&, int64_t a) { return a < 0 ? int64_t(0 - uint64_t(a)) : a; }
int32_t absvsi2(Ctx&, int32_t a) { return a < 0 ? int32_t(0 - uint32_t(a)) : a; }

// ---- bits
int32_t clzsi2(Ctx&, uint32_t a) { return a ? std::countl_zero(a) : 32; }
int32_t ctzsi2(Ctx&, uint32_t a) { return a ? std::countr_zero(a) : 32; }
int32_t clzdi2(Ctx&, uint64_t a) { return a ? std::countl_zero(a) : 64; }
int32_t ctzdi2(Ctx&, uint64_t a) { return a ? std::countr_zero(a) : 64; }
int32_t popcountsi2(Ctx&, uint32_t a) { return std::popcount(a); }
int32_t popcountdi2(Ctx&, uint64_t a) { return std::popcount(a); }
int32_t paritysi2(Ctx&, uint32_t a) { return std::popcount(a) & 1; }
int32_t paritydi2(Ctx&, uint64_t a) { return std::popcount(a) & 1; }
int32_t ffssi2(Ctx&, uint32_t a) { return a ? std::countr_zero(a) + 1 : 0; }
int32_t ffsdi2(Ctx&, uint64_t a) { return a ? std::countr_zero(a) + 1 : 0; }
uint32_t bswapsi2(Ctx&, uint32_t a) { return __builtin_bswap32(a); }
uint64_t bswapdi2(Ctx&, uint64_t a) { return __builtin_bswap64(a); }

// ---- conversions
double floatsidf(Ctx&, int32_t a) { return double(a); }
double floatunsidf(Ctx&, uint32_t a) { return double(a); }
double floatdidf(Ctx&, int64_t a) { return double(a); }
double floatundidf(Ctx&, uint64_t a) { return double(a); }
float floatsisf(Ctx&, int32_t a) { return float(a); }
float floatunsisf(Ctx&, uint32_t a) { return float(a); }
float floatdisf(Ctx&, int64_t a) { return float(a); }
float floatundisf(Ctx&, uint64_t a) { return float(a); }
int32_t fixdfsi(Ctx&, double a) { return satConv<int32_t>(a); }
uint32_t fixunsdfsi(Ctx&, double a) { return satConv<uint32_t>(a); }
int64_t fixdfdi(Ctx&, double a) { return satConv<int64_t>(a); }
uint64_t fixunsdfdi(Ctx&, double a) { return satConv<uint64_t>(a); }
int32_t fixsfsi(Ctx&, float a) { return satConv<int32_t>(a); }
uint32_t fixunssfsi(Ctx&, float a) { return satConv<uint32_t>(a); }
int64_t fixsfdi(Ctx&, float a) { return satConv<int64_t>(a); }
uint64_t fixunssfdi(Ctx&, float a) { return satConv<uint64_t>(a); }
double extendsfdf2(Ctx&, float a) { return double(a); }
float truncdfsf2(Ctx&, double a) { return float(a); }

// ---- soft-float arithmetic and comparisons
double adddf3(Ctx&, double a, double b) { return a + b; }
double subdf3(Ctx&, double a, double b) { return a - b; }
double muldf3(Ctx&, double a, double b) { return a * b; }
double divdf3(Ctx&, double a, double b) { return a / b; }
double negdf2(Ctx&, double a) { return -a; }
float addsf3(Ctx&, float a, float b) { return a + b; }
float subsf3(Ctx&, float a, float b) { return a - b; }
float mulsf3(Ctx&, float a, float b) { return a * b; }
float divsf3(Ctx&, float a, float b) { return a / b; }
float negsf2(Ctx&, float a) { return -a; }
// libgcc-style comparisons (__eqdf2 etc. return <0, 0, >0; unordered → 1, except __ge/__lt → -1)
template <typename F> int32_t cmpLE(F a, F b) { return std::isunordered(a, b) ? 1 : a < b ? -1 : a == b ? 0 : 1; }
template <typename F> int32_t cmpGE(F a, F b) { return std::isunordered(a, b) ? -1 : a < b ? -1 : a == b ? 0 : 1; }
int32_t eqdf2(Ctx&, double a, double b) { return cmpLE(a, b); }
int32_t ledf2(Ctx&, double a, double b) { return cmpLE(a, b); }
int32_t gedf2(Ctx&, double a, double b) { return cmpGE(a, b); }
int32_t unorddf2(Ctx&, double a, double b) { return std::isunordered(a, b); }
int32_t eqsf2(Ctx&, float a, float b) { return cmpLE(a, b); }
int32_t lesf2(Ctx&, float a, float b) { return cmpLE(a, b); }
int32_t gesf2(Ctx&, float a, float b) { return cmpGE(a, b); }
int32_t unordsf2(Ctx&, float a, float b) { return std::isunordered(a, b); }
// The Darwin `*vfp` predicates return a plain boolean.
int32_t eqdf2vfp(Ctx&, double a, double b) { return a == b; }
int32_t nedf2vfp(Ctx&, double a, double b) { return a != b; }
int32_t ltdf2vfp(Ctx&, double a, double b) { return a < b; }
int32_t ledf2vfp(Ctx&, double a, double b) { return a <= b; }
int32_t gtdf2vfp(Ctx&, double a, double b) { return a > b; }
int32_t gedf2vfp(Ctx&, double a, double b) { return a >= b; }
int32_t eqsf2vfp(Ctx&, float a, float b) { return a == b; }
int32_t nesf2vfp(Ctx&, float a, float b) { return a != b; }
int32_t ltsf2vfp(Ctx&, float a, float b) { return a < b; }
int32_t lesf2vfp(Ctx&, float a, float b) { return a <= b; }
int32_t gtsf2vfp(Ctx&, float a, float b) { return a > b; }
int32_t gesf2vfp(Ctx&, float a, float b) { return a >= b; }

} // namespace

void registerCompilerRt(Process& p) {
    p.def<&udivsi3>("___udivsi3");   p.def<&divsi3>("___divsi3");
    p.def<&umodsi3>("___umodsi3");   p.def<&modsi3>("___modsi3");
    p.def<&udivmodsi4>("___udivmodsi4"); p.def<&divmodsi4>("___divmodsi4");
    p.def<&udivdi3>("___udivdi3");   p.def<&divdi3>("___divdi3");
    p.def<&umoddi3>("___umoddi3");   p.def<&moddi3>("___moddi3");
    p.def<&udivmoddi4>("___udivmoddi4"); p.def<&divmoddi4>("___divmoddi4");
    p.def<&muldi3>("___muldi3");     p.def<&negdi2>("___negdi2");
    p.def<&ashldi3>("___ashldi3");   p.def<&lshrdi3>("___lshrdi3"); p.def<&ashrdi3>("___ashrdi3");
    p.def<&cmpdi2>("___cmpdi2");     p.def<&ucmpdi2>("___ucmpdi2");
    p.def<&absvdi2>("___absvdi2");   p.def<&absvsi2>("___absvsi2");

    p.def<&clzsi2>("___clzsi2");     p.def<&ctzsi2>("___ctzsi2");
    p.def<&clzdi2>("___clzdi2");     p.def<&ctzdi2>("___ctzdi2");
    p.def<&popcountsi2>("___popcountsi2"); p.def<&popcountdi2>("___popcountdi2");
    p.def<&paritysi2>("___paritysi2"); p.def<&paritydi2>("___paritydi2");
    p.def<&ffssi2>("___ffssi2");     p.def<&ffsdi2>("___ffsdi2");
    p.def<&bswapsi2>("___bswapsi2"); p.def<&bswapdi2>("___bswapdi2");

    // Each conversion / arithmetic helper exists as a plain and a `vfp` entry point.
    auto both = [&p](const std::string& base, uint32_t addr) {
        p.defineData("___" + base, addr);
        p.defineData("___" + base + "vfp", addr);
    };
    both("floatsidf", p.def<&floatsidf>("___hvm_floatsidf"));
    both("floatunsidf", p.def<&floatunsidf>("___hvm_floatunsidf"));
    p.defineData("___floatunssidfvfp", *p.lookupSymbol("___floatunsidf"));
    both("floatsisf", p.def<&floatsisf>("___hvm_floatsisf"));
    both("floatunsisf", p.def<&floatunsisf>("___hvm_floatunsisf"));
    p.defineData("___floatunssisfvfp", *p.lookupSymbol("___floatunsisf"));
    p.def<&floatdidf>("___floatdidf"); p.def<&floatundidf>("___floatundidf");
    p.def<&floatdisf>("___floatdisf"); p.def<&floatundisf>("___floatundisf");
    both("fixdfsi", p.def<&fixdfsi>("___hvm_fixdfsi"));
    both("fixunsdfsi", p.def<&fixunsdfsi>("___hvm_fixunsdfsi"));
    both("fixsfsi", p.def<&fixsfsi>("___hvm_fixsfsi"));
    both("fixunssfsi", p.def<&fixunssfsi>("___hvm_fixunssfsi"));
    p.def<&fixdfdi>("___fixdfdi"); p.def<&fixunsdfdi>("___fixunsdfdi");
    p.def<&fixsfdi>("___fixsfdi"); p.def<&fixunssfdi>("___fixunssfdi");
    both("extendsfdf2", p.def<&extendsfdf2>("___hvm_extendsfdf2"));
    both("truncdfsf2", p.def<&truncdfsf2>("___hvm_truncdfsf2"));

    both("adddf3", p.def<&adddf3>("___hvm_adddf3"));
    both("subdf3", p.def<&subdf3>("___hvm_subdf3"));
    both("muldf3", p.def<&muldf3>("___hvm_muldf3"));
    both("divdf3", p.def<&divdf3>("___hvm_divdf3"));
    both("negdf2", p.def<&negdf2>("___hvm_negdf2"));
    both("addsf3", p.def<&addsf3>("___hvm_addsf3"));
    both("subsf3", p.def<&subsf3>("___hvm_subsf3"));
    both("mulsf3", p.def<&mulsf3>("___hvm_mulsf3"));
    both("divsf3", p.def<&divsf3>("___hvm_divsf3"));
    both("negsf2", p.def<&negsf2>("___hvm_negsf2"));

    p.def<&eqdf2>("___eqdf2"); p.defineData("___nedf2", *p.lookupSymbol("___eqdf2"));
    p.def<&ledf2>("___ledf2"); p.defineData("___ltdf2", *p.lookupSymbol("___ledf2"));
    p.def<&gedf2>("___gedf2"); p.defineData("___gtdf2", *p.lookupSymbol("___gedf2"));
    p.def<&unorddf2>("___unorddf2"); p.defineData("___unorddf2vfp", *p.lookupSymbol("___unorddf2"));
    p.def<&eqsf2>("___eqsf2"); p.defineData("___nesf2", *p.lookupSymbol("___eqsf2"));
    p.def<&lesf2>("___lesf2"); p.defineData("___ltsf2", *p.lookupSymbol("___lesf2"));
    p.def<&gesf2>("___gesf2"); p.defineData("___gtsf2", *p.lookupSymbol("___gesf2"));
    p.def<&unordsf2>("___unordsf2"); p.defineData("___unordsf2vfp", *p.lookupSymbol("___unordsf2"));
    p.def<&eqdf2vfp>("___eqdf2vfp"); p.def<&nedf2vfp>("___nedf2vfp"); p.def<&ltdf2vfp>("___ltdf2vfp");
    p.def<&ledf2vfp>("___ledf2vfp"); p.def<&gtdf2vfp>("___gtdf2vfp"); p.def<&gedf2vfp>("___gedf2vfp");
    p.def<&eqsf2vfp>("___eqsf2vfp"); p.def<&nesf2vfp>("___nesf2vfp"); p.def<&ltsf2vfp>("___ltsf2vfp");
    p.def<&lesf2vfp>("___lesf2vfp"); p.def<&gtsf2vfp>("___gtsf2vfp"); p.def<&gesf2vfp>("___gesf2vfp");
}

} // namespace Hydrogen::Runtime
