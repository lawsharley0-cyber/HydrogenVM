// HydrogenVM — <math.h>. Uses the host libm (iOS passes floats in core
// registers, which the ABI wrapper takes care of).
#include "runtime/Process.hpp"
#include <cmath>
#include <cstring>

namespace Hydrogen::Runtime {
namespace {

#define D1(name, expr) double d_##name(Ctx&, double a) { return expr; }
#define D2(name, expr) double d_##name(Ctx&, double a, double b) { return expr; }
#define F1(name, expr) float f_##name(Ctx&, float a) { return expr; }
#define F2(name, expr) float f_##name(Ctx&, float a, float b) { return expr; }

D1(sin, std::sin(a)) D1(cos, std::cos(a)) D1(tan, std::tan(a)) D1(asin, std::asin(a)) D1(acos, std::acos(a))
D1(atan, std::atan(a)) D1(sinh, std::sinh(a)) D1(cosh, std::cosh(a)) D1(tanh, std::tanh(a)) D1(asinh, std::asinh(a))
D1(acosh, std::acosh(a)) D1(atanh, std::atanh(a)) D1(exp, std::exp(a)) D1(exp2, std::exp2(a)) D1(expm1, std::expm1(a))
D1(log, std::log(a)) D1(log2, std::log2(a)) D1(log10, std::log10(a)) D1(log1p, std::log1p(a)) D1(sqrt, std::sqrt(a))
D1(cbrt, std::cbrt(a)) D1(fabs, std::fabs(a)) D1(floor, std::floor(a)) D1(ceil, std::ceil(a)) D1(round, std::round(a))
D1(trunc, std::trunc(a)) D1(rint, std::rint(a)) D1(nearbyint, std::nearbyint(a)) D1(logb, std::logb(a)) D1(erf, std::erf(a))
D1(erfc, std::erfc(a)) D1(tgamma, std::tgamma(a)) D1(lgamma, std::lgamma(a))
D2(atan2, std::atan2(a, b)) D2(pow, std::pow(a, b)) D2(fmod, std::fmod(a, b)) D2(hypot, std::hypot(a, b))
D2(fmin, std::fmin(a, b)) D2(fmax, std::fmax(a, b)) D2(copysign, std::copysign(a, b)) D2(remainder, std::remainder(a, b))
D2(fdim, std::fdim(a, b)) D2(nextafter, std::nextafter(a, b))
F1(sin, std::sin(a)) F1(cos, std::cos(a)) F1(tan, std::tan(a)) F1(asin, std::asin(a)) F1(acos, std::acos(a))
F1(atan, std::atan(a)) F1(sinh, std::sinh(a)) F1(cosh, std::cosh(a)) F1(tanh, std::tanh(a)) F1(exp, std::exp(a))
F1(exp2, std::exp2(a)) F1(log, std::log(a)) F1(log2, std::log2(a)) F1(log10, std::log10(a)) F1(sqrt, std::sqrt(a))
F1(cbrt, std::cbrt(a)) F1(fabs, std::fabs(a)) F1(floor, std::floor(a)) F1(ceil, std::ceil(a)) F1(round, std::round(a))
F1(trunc, std::trunc(a)) F1(rint, std::rint(a)) F1(log1p, std::log1p(a)) F1(expm1, std::expm1(a)) F1(nearbyint, std::nearbyint(a))
F2(atan2, std::atan2(a, b)) F2(pow, std::pow(a, b)) F2(fmod, std::fmod(a, b)) F2(hypot, std::hypot(a, b))
F2(fmin, std::fmin(a, b)) F2(fmax, std::fmax(a, b)) F2(copysign, std::copysign(a, b)) F2(remainder, std::remainder(a, b))

int32_t lround_d(Ctx&, double a) { return int32_t(std::lround(a)); }
int32_t lround_f(Ctx&, float a) { return int32_t(std::lround(a)); }
int32_t lrint_d(Ctx&, double a) { return int32_t(std::lrint(a)); }
int32_t lrint_f(Ctx&, float a) { return int32_t(std::lrint(a)); }
int64_t llround_d(Ctx&, double a) { return std::llround(a); }
double ldexp_d(Ctx&, double a, int32_t e) { return std::ldexp(a, e); }
float ldexp_f(Ctx&, float a, int32_t e) { return std::ldexp(a, e); }
double frexp_d(Ctx& x, double a, GuestPtr e) { int ex; const double r = std::frexp(a, &ex); x.mem.write<int32_t>(e, ex); return r; }
float frexp_f(Ctx& x, float a, GuestPtr e) { int ex; const float r = std::frexp(a, &ex); x.mem.write<int32_t>(e, ex); return r; }
double modf_d(Ctx& x, double a, GuestPtr ip) { double i; const double r = std::modf(a, &i); uint64_t b; std::memcpy(&b, &i, 8); x.mem.write<uint64_t>(ip, b); return r; }
float modf_f(Ctx& x, float a, GuestPtr ip) { float i; const float r = std::modf(a, &i); uint32_t b; std::memcpy(&b, &i, 4); x.mem.write<uint32_t>(ip, b); return r; }
void sincos_d(Ctx& x, double a, GuestPtr s, GuestPtr c) {
    const double sv = std::sin(a), cv = std::cos(a);
    uint64_t b; std::memcpy(&b, &sv, 8); x.mem.write<uint64_t>(s, b); std::memcpy(&b, &cv, 8); x.mem.write<uint64_t>(c, b);
}
void sincos_f(Ctx& x, float a, GuestPtr s, GuestPtr c) {
    const float sv = std::sin(a), cv = std::cos(a);
    uint32_t b; std::memcpy(&b, &sv, 4); x.mem.write<uint32_t>(s, b); std::memcpy(&b, &cv, 4); x.mem.write<uint32_t>(c, b);
}
// 32-bit iOS APCS: struct __sincosf_ret { float __sinval, __cosval; } is 8 bytes, returned in r0:r1.
uint64_t sincosf_stret(Ctx&, float a) {
    const float sv = std::sin(a), cv = std::cos(a);
    uint32_t sb, cb;
    std::memcpy(&sb, &sv, 4);
    std::memcpy(&cb, &cv, 4);
    return uint64_t(sb) | (uint64_t(cb) << 32);
}
// struct __sincos_ret { double __sinval, __cosval; } is 16 bytes, returned via sret pointer in r0.
void sincos_stret(Ctx& x, GuestPtr out, double a) {
    const double sv = std::sin(a), cv = std::cos(a);
    uint64_t sb, cb;
    std::memcpy(&sb, &sv, 8);
    std::memcpy(&cb, &cv, 8);
    x.mem.write<uint64_t>(out, sb);
    x.mem.write<uint64_t>(out + 8, cb);
}
double exp10_d(Ctx&, double a) { return std::pow(10.0, a); }
float exp10_f(Ctx&, float a) { return std::pow(10.0f, a); }
int32_t isnan_d(Ctx&, double a) { return std::isnan(a); }
int32_t isinf_d(Ctx&, double a) { return std::isinf(a) ? (a > 0 ? 1 : -1) : 0; }
int32_t isnan_f(Ctx&, float a) { return std::isnan(a); }
int32_t isinf_f(Ctx&, float a) { return std::isinf(a) ? (a > 0 ? 1 : -1) : 0; }
int32_t fpclassify_d(Ctx&, double a) {
    // Darwin FP_* values: NAN 1, INFINITE 2, ZERO 3, NORMAL 4, SUBNORMAL 5
    switch (std::fpclassify(a)) { case FP_NAN: return 1; case FP_INFINITE: return 2; case FP_ZERO: return 3; case FP_SUBNORMAL: return 5; default: return 4; }
}
int32_t fpclassify_f(Ctx& x, float a) { return fpclassify_d(x, a); }

} // namespace

void registerLibcMath(Process& p) {
#define RD(n) p.def<&d_##n>("_" #n);
#define RF(n) p.def<&f_##n>("_" #n "f");
    RD(sin) RD(cos) RD(tan) RD(asin) RD(acos) RD(atan) RD(sinh) RD(cosh) RD(tanh) RD(asinh) RD(acosh) RD(atanh)
    RD(exp) RD(exp2) RD(expm1) RD(log) RD(log2) RD(log10) RD(log1p) RD(sqrt) RD(cbrt) RD(fabs) RD(floor) RD(ceil)
    RD(round) RD(trunc) RD(rint) RD(nearbyint) RD(logb) RD(erf) RD(erfc) RD(tgamma) RD(lgamma)
    RD(atan2) RD(pow) RD(fmod) RD(hypot) RD(fmin) RD(fmax) RD(copysign) RD(remainder) RD(fdim) RD(nextafter)
    RF(sin) RF(cos) RF(tan) RF(asin) RF(acos) RF(atan) RF(sinh) RF(cosh) RF(tanh) RF(exp) RF(exp2) RF(log) RF(log2)
    RF(log10) RF(sqrt) RF(cbrt) RF(fabs) RF(floor) RF(ceil) RF(round) RF(trunc) RF(rint) RF(log1p) RF(expm1) RF(nearbyint)
    RF(atan2) RF(pow) RF(fmod) RF(hypot) RF(fmin) RF(fmax) RF(copysign) RF(remainder)
#undef RD
#undef RF
    p.def<&lround_d>("_lround");
    p.def<&lround_f>("_lroundf");
    p.def<&lrint_d>("_lrint");
    p.def<&lrint_f>("_lrintf");
    p.def<&llround_d>("_llround");
    p.def<&ldexp_d>("_ldexp");
    p.def<&ldexp_d>("_scalbn");
    p.def<&ldexp_f>("_ldexpf");
    p.def<&ldexp_f>("_scalbnf");
    p.def<&frexp_d>("_frexp");
    p.def<&frexp_f>("_frexpf");
    p.def<&modf_d>("_modf");
    p.def<&modf_f>("_modff");
    p.def<&sincos_d>("___sincos");
    p.def<&sincos_f>("___sincosf");
    p.def<&sincos_stret>("___sincos_stret");
    p.def<&sincosf_stret>("___sincosf_stret");
    p.def<&exp10_d>("___exp10");
    p.def<&exp10_f>("___exp10f");
    p.def<&isnan_d>("___isnand");
    p.def<&isinf_d>("___isinfd");
    p.def<&isnan_f>("___isnanf");
    p.def<&isinf_f>("___isinff");
    p.def<&fpclassify_d>("___fpclassifyd");
    p.def<&fpclassify_f>("___fpclassifyf");
    p.def<&d_sqrt>("___sqrt");
}

} // namespace Hydrogen::Runtime
