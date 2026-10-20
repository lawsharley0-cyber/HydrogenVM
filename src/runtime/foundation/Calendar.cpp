// HydrogenVM — NSCalendar (Gregorian) and NSDateComponents.
//
// Dates are NSDate reference-date seconds; the calendar turns them into
// wall-clock fields in its time zone (the device's, unless the app sets one)
// with proleptic-Gregorian day arithmetic, so the results don't depend on the
// host's calendar support. Other calendar identifiers are accepted and
// computed as Gregorian.
#include "Foundation.hpp"
#include <cmath>
#include <ctime>

namespace Hydrogen::Runtime::Foundation {
namespace {

using ObjC::Class;
using ObjC::SEL;
ObjC::Runtime& R(Ctx& x) { return ObjC::rt(x); }

constexpr int32_t kUndefined = 0x7FFFFFFF;   // NSDateComponentUndefined (NSIntegerMax)
constexpr double kUnixToRef = 978307200.0;
enum Unit : uint32_t {
    uEra = 2, uYear = 4, uMonth = 8, uDay = 16, uHour = 32, uMinute = 64, uSecond = 128, uWeek = 256, uWeekday = 512,
    uWeekdayOrdinal = 1024, uQuarter = 2048, uWeekOfMonth = 4096, uWeekOfYear = 8192, uYearForWeekOfYear = 16384,
    uNanosecond = 32768, uCalendar = 1u << 20, uTimeZone = 1u << 21,
};

// Field order in DateComponents::v.
enum Field { fEra, fYear, fMonth, fDay, fHour, fMinute, fSecond, fNanosecond, fWeekday, fWeekdayOrdinal, fQuarter,
             fWeekOfMonth, fWeekOfYear, fYearForWeekOfYear, fWeek, kFields };
struct Components : ObjC::HostObject {
    int32_t v[kFields];
    id calendar = 0, timeZone = 0;
    bool leapMonth = false;
    Components() { for (int32_t& f : v) f = kUndefined; }
};
struct CalendarData : ObjC::HostObject {
    id identifier = 0, timeZone = 0, locale = 0;
    int32_t firstWeekday = 1, minDays = 1;
};
struct CalState {
    explicit CalState(Process&) {}
    Class calendar = 0, components = 0;
    id current = 0;
};
CalState& CS(Ctx& x) { return x.proc.service<CalState>(); }

// ---- civil date arithmetic (Howard Hinnant's algorithms)
int64_t daysFromCivil(int64_t y, int64_t m, int64_t d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}
void civilFromDays(int64_t z, int64_t& y, int64_t& m, int64_t& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y += m <= 2;
}
bool leap(int64_t y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }
int daysInMonth(int64_t y, int64_t m) { static const int d[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31}; return m == 2 && leap(y) ? 29 : d[m - 1]; }
int64_t floorDiv(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

int32_t offsetAt(Ctx& x, CalendarData* c, double ref) {
    if (c && c->timeZone) return int32_t(R(x).send(x.cpu, c->timeZone, "secondsFromGMTForDate:", {makeDate(x, ref)}));
    const time_t t = time_t(std::floor(ref + kUnixToRef));
    std::tm lt;
    localtime_r(&t, &lt);
    return int32_t(lt.tm_gmtoff);
}

struct Broken { int64_t year, month, day, hour, minute, second, nano, weekday, dayOfYear, days; };
Broken breakDown(Ctx& x, CalendarData* c, double ref) {
    const double local = ref + kUnixToRef + offsetAt(x, c, ref);
    const double whole = std::floor(local);
    const int64_t secs = int64_t(whole);
    Broken b{};
    b.days = floorDiv(secs, 86400);
    const int64_t sod = secs - b.days * 86400;
    civilFromDays(b.days, b.year, b.month, b.day);
    b.hour = sod / 3600;
    b.minute = sod / 60 % 60;
    b.second = sod % 60;
    b.nano = int64_t(std::llround((local - whole) * 1e9));
    b.weekday = ((b.days % 7) + 7 + 4) % 7 + 1;   // 1970-01-01 was a Thursday; 1 = Sunday
    b.dayOfYear = b.days - daysFromCivil(b.year, 1, 1) + 1;
    return b;
}
// Local wall-clock fields -> reference-date seconds (the offset is resolved at the result).
double compose(Ctx& x, CalendarData* c, int64_t y, int64_t mo, int64_t d, int64_t h, int64_t mi, double s) {
    y += floorDiv(mo - 1, 12);
    mo = mo - 1 - floorDiv(mo - 1, 12) * 12 + 1;
    const double localUnix = double(daysFromCivil(y, mo, 1) + (d - 1)) * 86400.0 + double(h) * 3600 + double(mi) * 60 + s;
    double ref = localUnix - kUnixToRef;
    // Two passes settle DST transitions.
    for (int i = 0; i < 2; ++i) ref = localUnix - kUnixToRef - offsetAt(x, c, ref);
    return ref;
}
int64_t weekOfYear(const Broken& b, CalendarData* c) {
    const int fw = c ? c->firstWeekday : 1;
    const int64_t jan1Weekday = ((b.days - b.dayOfYear + 1) % 7 + 7 + 4) % 7 + 1;
    const int64_t shift = (jan1Weekday - fw + 7) % 7;
    return (b.dayOfYear - 1 + shift) / 7 + 1;
}

CalendarData* cal(Ctx& x, id c) { return c ? R(x).hostIf<CalendarData>(c) : nullptr; }
Components* comps(Ctx& x, id c) { return c ? R(x).hostIf<Components>(c) : nullptr; }

id newComponents(Ctx& x, bool owned) {
    const id o = R(x).createInstance(CS(x).components);
    R(x).setHost(o, std::make_unique<Components>());
    return owned ? o : autoreleased(x, o);
}

// ---- NSDateComponents
template <int F> int32_t dc_get(Ctx& x, id self, SEL) { Components* c = comps(x, self); return c ? c->v[F] : kUndefined; }
template <int F> void dc_set(Ctx& x, id self, SEL, int32_t v) { if (Components* c = comps(x, self)) c->v[F] = v; }
id dc_init(Ctx& x, id self, SEL) { if (!comps(x, self)) R(x).setHost(self, std::make_unique<Components>()); return self; }
id dc_calendar(Ctx& x, id self, SEL) { Components* c = comps(x, self); return c ? c->calendar : 0; }
void dc_setCalendar(Ctx& x, id self, SEL, id v) {
    Components* c = comps(x, self);
    if (!c) return;
    const id old = c->calendar;
    c->calendar = v ? R(x).retain(x.cpu, v) : 0;
    if (old) R(x).release(x.cpu, old);
}
id dc_timeZone(Ctx& x, id self, SEL) { Components* c = comps(x, self); return c ? c->timeZone : 0; }
void dc_setTimeZone(Ctx& x, id self, SEL, id v) {
    Components* c = comps(x, self);
    if (!c) return;
    const id old = c->timeZone;
    c->timeZone = v ? R(x).retain(x.cpu, v) : 0;
    if (old) R(x).release(x.cpu, old);
}
bool dc_isLeapMonth(Ctx& x, id self, SEL) { Components* c = comps(x, self); return c && c->leapMonth; }
void dc_setLeapMonth(Ctx& x, id self, SEL, bool v) { if (Components* c = comps(x, self)) c->leapMonth = v; }
id dc_copy(Ctx& x, id self, SEL, uint32_t) {
    const id o = newComponents(x, true);
    if (Components* a = comps(x, self)) {
        Components* b = comps(x, o);
        std::copy(std::begin(a->v), std::end(a->v), std::begin(b->v));
        if (a->calendar) b->calendar = R(x).retain(x.cpu, a->calendar);
        if (a->timeZone) b->timeZone = R(x).retain(x.cpu, a->timeZone);
    }
    return o;
}
void dc_dealloc(Ctx& x, id self, SEL) {
    if (Components* c = comps(x, self)) {
        if (c->calendar) R(x).release(x.cpu, c->calendar);
        if (c->timeZone) R(x).release(x.cpu, c->timeZone);
    }
    R(x).disposeInstance(x.cpu, self);
}
id cal_dateFromComponents(Ctx& x, id self, SEL, id comp);
id dc_date(Ctx& x, id self, SEL s) {
    Components* c = comps(x, self);
    if (!c) return 0;
    const id calendar = c->calendar ? c->calendar : id(R(x).send(x.cpu, CS(x).calendar, "currentCalendar"));
    return cal_dateFromComponents(x, calendar, s, self);
}

// ---- NSCalendar
id newCalendar(Ctx& x, id identifier, bool owned) {
    const id o = R(x).createInstance(CS(x).calendar);
    auto h = std::make_unique<CalendarData>();
    h->identifier = identifier ? id(R(x).send(x.cpu, identifier, "copy")) : constString(x.proc, "gregorian");
    R(x).setHost(o, std::move(h));
    return owned ? o : autoreleased(x, o);
}
id calc_current(Ctx& x, id, SEL) {
    auto& s = CS(x);
    if (!s.current) {
        s.current = newCalendar(x, constString(x.proc, "gregorian"), true);
        R(x).makeImmortal(s.current);
    }
    return s.current;
}
id calc_withIdentifier(Ctx& x, id, SEL, id ident) { return newCalendar(x, ident, false); }
id cal_initWithIdentifier(Ctx& x, id self, SEL, id ident) {
    auto h = std::make_unique<CalendarData>();
    h->identifier = ident ? id(R(x).send(x.cpu, ident, "copy")) : constString(x.proc, "gregorian");
    R(x).setHost(self, std::move(h));
    return self;
}
id cal_init(Ctx& x, id self, SEL s) { return cal_initWithIdentifier(x, self, s, 0); }
id cal_identifier(Ctx& x, id self, SEL) { CalendarData* c = cal(x, self); return c ? c->identifier : constString(x.proc, "gregorian"); }
id cal_timeZone(Ctx& x, id self, SEL) {
    CalendarData* c = cal(x, self);
    if (c && c->timeZone) return c->timeZone;
    return id(R(x).send(x.cpu, R(x).getClass("NSTimeZone"), "localTimeZone"));
}
void cal_setTimeZone(Ctx& x, id self, SEL, id tz) {
    CalendarData* c = cal(x, self);
    if (!c || self == CS(x).current) return;   // the shared current calendar stays local
    const id old = c->timeZone;
    c->timeZone = tz ? R(x).retain(x.cpu, tz) : 0;
    if (old) R(x).release(x.cpu, old);
}
id cal_locale(Ctx& x, id self, SEL) {
    CalendarData* c = cal(x, self);
    return c && c->locale ? c->locale : id(R(x).send(x.cpu, R(x).getClass("NSLocale"), "currentLocale"));
}
void cal_setLocale(Ctx& x, id self, SEL, id l) {
    CalendarData* c = cal(x, self);
    if (!c) return;
    const id old = c->locale;
    c->locale = l ? R(x).retain(x.cpu, l) : 0;
    if (old) R(x).release(x.cpu, old);
}
int32_t cal_firstWeekday(Ctx& x, id self, SEL) { CalendarData* c = cal(x, self); return c ? c->firstWeekday : 1; }
void cal_setFirstWeekday(Ctx& x, id self, SEL, int32_t v) { if (CalendarData* c = cal(x, self)) c->firstWeekday = std::clamp(v, 1, 7); }
int32_t cal_minDays(Ctx& x, id self, SEL) { CalendarData* c = cal(x, self); return c ? c->minDays : 1; }
void cal_setMinDays(Ctx& x, id self, SEL, int32_t v) { if (CalendarData* c = cal(x, self)) c->minDays = std::clamp(v, 1, 7); }

id cal_components(Ctx& x, id self, SEL, uint32_t units, id date) {
    if (!date) return 0;
    CalendarData* c = cal(x, self);
    const Broken b = breakDown(x, c, dateInterval(x, date));
    const id o = newComponents(x, false);
    Components* k = comps(x, o);
    if (units & uEra) k->v[fEra] = b.year > 0 ? 1 : 0;
    if (units & uYear) k->v[fYear] = int32_t(b.year);
    if (units & uMonth) k->v[fMonth] = int32_t(b.month);
    if (units & uDay) k->v[fDay] = int32_t(b.day);
    if (units & uHour) k->v[fHour] = int32_t(b.hour);
    if (units & uMinute) k->v[fMinute] = int32_t(b.minute);
    if (units & uSecond) k->v[fSecond] = int32_t(b.second);
    if (units & uNanosecond) k->v[fNanosecond] = int32_t(b.nano);
    if (units & uWeekday) k->v[fWeekday] = int32_t(b.weekday);
    if (units & uWeekdayOrdinal) k->v[fWeekdayOrdinal] = int32_t((b.day - 1) / 7 + 1);
    if (units & uQuarter) k->v[fQuarter] = int32_t((b.month - 1) / 3 + 1);
    if (units & (uWeekOfYear | uWeek)) {
        const int32_t w = int32_t(weekOfYear(b, c));
        if (units & uWeekOfYear) k->v[fWeekOfYear] = w;
        if (units & uWeek) k->v[fWeek] = w;
    }
    if (units & uWeekOfMonth) {
        const int fw = c ? c->firstWeekday : 1;
        const int64_t firstWd = ((b.days - b.day + 1) % 7 + 7 + 4) % 7 + 1;
        k->v[fWeekOfMonth] = int32_t((b.day - 1 + (firstWd - fw + 7) % 7) / 7 + 1);
    }
    if (units & uYearForWeekOfYear) k->v[fYearForWeekOfYear] = int32_t(b.year);
    if (units & uCalendar) k->calendar = R(x).retain(x.cpu, self);
    if (units & uTimeZone) k->timeZone = R(x).retain(x.cpu, cal_timeZone(x, self, 0));
    return o;
}
int32_t field(Components* k, int f, int32_t def) { return k->v[f] == kUndefined ? def : k->v[f]; }
id cal_dateFromComponents(Ctx& x, id self, SEL, id comp) {
    Components* k = comps(x, comp);
    if (!k) return 0;
    CalendarData* c = cal(x, self);
    const double ns = k->v[fNanosecond] == kUndefined ? 0.0 : k->v[fNanosecond] * 1e-9;
    double t = compose(x, c, field(k, fYear, 1), field(k, fMonth, 1), field(k, fDay, 1), field(k, fHour, 0), field(k, fMinute, 0),
                       field(k, fSecond, 0) + ns);
    // A weekday (with optional week of year) instead of a day.
    if (k->v[fDay] == kUndefined && k->v[fWeekday] != kUndefined) {
        const Broken b = breakDown(x, c, t);
        int64_t delta = k->v[fWeekday] - b.weekday;
        if (k->v[fWeekOfYear] != kUndefined) delta += int64_t(k->v[fWeekOfYear] - weekOfYear(b, c)) * 7;
        else if (k->v[fWeekdayOrdinal] != kUndefined) { if (delta < 0) delta += 7; delta += int64_t(k->v[fWeekdayOrdinal] - 1) * 7; }
        t = compose(x, c, b.year, b.month, b.day + delta, b.hour, b.minute, double(b.second) + ns);
    }
    return makeDate(x, t);
}
double addTo(Ctx& x, CalendarData* c, double t, Components* k, int sign) {
    const Broken b = breakDown(x, c, t);
    int64_t y = b.year, mo = b.month, d = b.day;
    if (k->v[fYear] != kUndefined) y += sign * int64_t(k->v[fYear]);
    if (k->v[fMonth] != kUndefined) mo += sign * int64_t(k->v[fMonth]);
    if (k->v[fQuarter] != kUndefined) mo += sign * int64_t(k->v[fQuarter]) * 3;
    y += floorDiv(mo - 1, 12);
    mo = mo - 1 - floorDiv(mo - 1, 12) * 12 + 1;
    d = std::min<int64_t>(d, daysInMonth(y, mo));   // Jan 31 + 1 month = Feb 28/29
    if (k->v[fDay] != kUndefined) d += sign * int64_t(k->v[fDay]);
    for (int f : {fWeek, fWeekOfYear, fWeekOfMonth}) if (k->v[f] != kUndefined) d += sign * int64_t(k->v[f]) * 7;
    if (k->v[fWeekday] != kUndefined) d += sign * int64_t(k->v[fWeekday]);
    double r = compose(x, c, y, mo, d, b.hour, b.minute, double(b.second) + double(b.nano) * 1e-9);
    if (k->v[fHour] != kUndefined) r += sign * 3600.0 * k->v[fHour];
    if (k->v[fMinute] != kUndefined) r += sign * 60.0 * k->v[fMinute];
    if (k->v[fSecond] != kUndefined) r += sign * double(k->v[fSecond]);
    if (k->v[fNanosecond] != kUndefined) r += sign * k->v[fNanosecond] * 1e-9;
    return r;
}
id cal_dateByAdding(Ctx& x, id self, SEL, id comp, id date, uint32_t) {
    Components* k = comps(x, comp);
    if (!k || !date) return 0;
    return makeDate(x, addTo(x, cal(x, self), dateInterval(x, date), k, 1));
}
// dateByAddingUnit:value:toDate:options: (iOS 8)
id cal_dateByAddingUnit(Ctx& x, id self, SEL, uint32_t unit, int32_t value, id date, uint32_t) {
    if (!date) return 0;
    Components k;
    const std::pair<uint32_t, int> map[] = {{uYear, fYear}, {uMonth, fMonth}, {uDay, fDay}, {uHour, fHour}, {uMinute, fMinute},
                                            {uSecond, fSecond}, {uNanosecond, fNanosecond}, {uWeekOfYear, fWeekOfYear}, {uWeek, fWeek},
                                            {uWeekday, fWeekday}, {uQuarter, fQuarter}, {uWeekOfMonth, fWeekOfMonth}};
    for (auto& [u, f] : map) if (unit == u) k.v[f] = value;
    return makeDate(x, addTo(x, cal(x, self), dateInterval(x, date), &k, 1));
}
// components:fromDate:toDate:options: — the largest requested units first.
id cal_difference(Ctx& x, id self, SEL, uint32_t units, id from, id to, uint32_t) {
    if (!from || !to) return 0;
    CalendarData* c = cal(x, self);
    double a = dateInterval(x, from);
    const double b = dateInterval(x, to);
    const int sign = b >= a ? 1 : -1;
    const id o = newComponents(x, false);
    Components* out = comps(x, o);
    auto stepUnits = [&](int f) {
        // Largest n with a + n units not passing b (calendar arithmetic for years/months).
        int64_t n = 0;
        for (int64_t step = 1 << 20; step; step >>= 1) {
            Components k;
            k.v[f] = int32_t(sign * (n + step));
            const double t = addTo(x, c, a, &k, 1);
            if (sign > 0 ? t <= b : t >= b) n += step;
        }
        Components k;
        k.v[f] = int32_t(sign * n);
        a = addTo(x, c, a, &k, 1);
        out->v[f] = int32_t(sign * n);
    };
    if (units & uEra) out->v[fEra] = 0;
    if (units & uYear) stepUnits(fYear);
    if (units & uMonth) stepUnits(fMonth);
    if (units & (uWeekOfYear | uWeek)) stepUnits(units & uWeekOfYear ? fWeekOfYear : fWeek);
    if (units & uDay) stepUnits(fDay);
    double rest = b - a;
    auto take = [&](uint32_t u, int f, double size) {
        if (!(units & u)) return;
        const double n = std::trunc(rest / size);
        out->v[f] = int32_t(n);
        rest -= n * size;
    };
    take(uHour, fHour, 3600);
    take(uMinute, fMinute, 60);
    take(uSecond, fSecond, 1);
    if (units & uNanosecond) out->v[fNanosecond] = int32_t(std::llround(rest * 1e9));
    return o;
}
// ordinalityOfUnit:inUnit:forDate:
uint32_t cal_ordinality(Ctx& x, id self, SEL, uint32_t smaller, uint32_t larger, id date) {
    if (!date) return uint32_t(kUndefined);
    CalendarData* c = cal(x, self);
    const Broken b = breakDown(x, c, dateInterval(x, date));
    if (smaller == uDay && larger == uMonth) return uint32_t(b.day);
    if (smaller == uDay && larger == uYear) return uint32_t(b.dayOfYear);
    if (smaller == uDay && (larger == uWeek || larger == uWeekOfYear || larger == uWeekOfMonth))
        return uint32_t((b.weekday - (c ? c->firstWeekday : 1) + 7) % 7 + 1);
    if (smaller == uWeekday && (larger == uWeek || larger == uWeekOfYear || larger == uWeekOfMonth))
        return uint32_t((b.weekday - (c ? c->firstWeekday : 1) + 7) % 7 + 1);
    if (smaller == uMonth && larger == uYear) return uint32_t(b.month);
    if (smaller == uHour && larger == uDay) return uint32_t(b.hour + 1);
    if (smaller == uMinute && larger == uHour) return uint32_t(b.minute + 1);
    if (smaller == uSecond && larger == uMinute) return uint32_t(b.second + 1);
    if ((smaller == uWeek || smaller == uWeekOfYear) && larger == uYear) return uint32_t(weekOfYear(b, c));
    if (smaller == uDay && larger == uEra) return uint32_t(b.days + 719163);
    return uint32_t(kUndefined);
}
// iOS 8 conveniences
int32_t cal_component(Ctx& x, id self, SEL s, uint32_t unit, id date) {
    const id k = cal_components(x, self, s, unit, date);
    Components* c = comps(x, k);
    if (!c) return kUndefined;
    for (int f = 0; f < kFields; ++f) if (c->v[f] != kUndefined) return c->v[f];
    return kUndefined;
}
id cal_startOfDay(Ctx& x, id self, SEL, id date) {
    if (!date) return 0;
    CalendarData* c = cal(x, self);
    const Broken b = breakDown(x, c, dateInterval(x, date));
    return makeDate(x, compose(x, c, b.year, b.month, b.day, 0, 0, 0));
}
bool cal_isDateInToday(Ctx& x, id self, SEL, id date) {
    if (!date) return false;
    CalendarData* c = cal(x, self);
    return breakDown(x, c, dateInterval(x, date)).days == breakDown(x, c, now()).days;
}
bool cal_isSameDay(Ctx& x, id self, SEL, id a, id b) {
    if (!a || !b) return false;
    CalendarData* c = cal(x, self);
    return breakDown(x, c, dateInterval(x, a)).days == breakDown(x, c, dateInterval(x, b)).days;
}
id cal_copy(Ctx& x, id self, SEL, uint32_t) {
    const id o = newCalendar(x, cal_identifier(x, self, 0), true);
    if (CalendarData* a = cal(x, self)) {
        CalendarData* b = cal(x, o);
        b->firstWeekday = a->firstWeekday;
        b->minDays = a->minDays;
        if (a->timeZone) b->timeZone = R(x).retain(x.cpu, a->timeZone);
    }
    return o;
}
void cal_dealloc(Ctx& x, id self, SEL) {
    if (CalendarData* c = cal(x, self)) {
        for (id o : {c->timeZone, c->locale}) if (o) R(x).release(x.cpu, o);
    }
    R(x).disposeInstance(x.cpu, self);
}

} // namespace

void registerCalendar(Process& p, ObjC::Runtime& r) {
    CalState& s = p.service<CalState>();
    ObjC::ClassBuilder dc = r.defineClass("NSDateComponents", "NSObject");
    s.components = dc.cls();
    dc.m<&dc_init>("init")
      .m<&dc_get<fEra>>("era", "i@:").m<&dc_set<fEra>>("setEra:", "v@:i")
      .m<&dc_get<fYear>>("year", "i@:").m<&dc_set<fYear>>("setYear:", "v@:i")
      .m<&dc_get<fMonth>>("month", "i@:").m<&dc_set<fMonth>>("setMonth:", "v@:i")
      .m<&dc_get<fDay>>("day", "i@:").m<&dc_set<fDay>>("setDay:", "v@:i")
      .m<&dc_get<fHour>>("hour", "i@:").m<&dc_set<fHour>>("setHour:", "v@:i")
      .m<&dc_get<fMinute>>("minute", "i@:").m<&dc_set<fMinute>>("setMinute:", "v@:i")
      .m<&dc_get<fSecond>>("second", "i@:").m<&dc_set<fSecond>>("setSecond:", "v@:i")
      .m<&dc_get<fNanosecond>>("nanosecond", "i@:").m<&dc_set<fNanosecond>>("setNanosecond:", "v@:i")
      .m<&dc_get<fWeekday>>("weekday", "i@:").m<&dc_set<fWeekday>>("setWeekday:", "v@:i")
      .m<&dc_get<fWeekdayOrdinal>>("weekdayOrdinal", "i@:").m<&dc_set<fWeekdayOrdinal>>("setWeekdayOrdinal:", "v@:i")
      .m<&dc_get<fQuarter>>("quarter", "i@:").m<&dc_set<fQuarter>>("setQuarter:", "v@:i")
      .m<&dc_get<fWeekOfMonth>>("weekOfMonth", "i@:").m<&dc_set<fWeekOfMonth>>("setWeekOfMonth:", "v@:i")
      .m<&dc_get<fWeekOfYear>>("weekOfYear", "i@:").m<&dc_set<fWeekOfYear>>("setWeekOfYear:", "v@:i")
      .m<&dc_get<fYearForWeekOfYear>>("yearForWeekOfYear", "i@:").m<&dc_set<fYearForWeekOfYear>>("setYearForWeekOfYear:", "v@:i")
      .m<&dc_get<fWeek>>("week", "i@:").m<&dc_set<fWeek>>("setWeek:", "v@:i")
      .m<&dc_calendar>("calendar").m<&dc_setCalendar>("setCalendar:", "v@:@")
      .m<&dc_timeZone>("timeZone").m<&dc_setTimeZone>("setTimeZone:", "v@:@")
      .m<&dc_isLeapMonth>("isLeapMonth", "c@:").m<&dc_setLeapMonth>("setLeapMonth:", "v@:c")
      .m<&dc_date>("date").m<&dc_copy>("copyWithZone:").m<&dc_dealloc>("dealloc", "v@:");

    ObjC::ClassBuilder c = r.defineClass("NSCalendar", "NSObject");
    s.calendar = c.cls();
    c.cm<&calc_current>("currentCalendar").cm<&calc_current>("autoupdatingCurrentCalendar")
     .cm<&calc_withIdentifier>("calendarWithIdentifier:");
    c.m<&cal_init>("init").m<&cal_initWithIdentifier>("initWithCalendarIdentifier:")
     .m<&cal_identifier>("calendarIdentifier").m<&cal_timeZone>("timeZone").m<&cal_setTimeZone>("setTimeZone:", "v@:@")
     .m<&cal_locale>("locale").m<&cal_setLocale>("setLocale:", "v@:@")
     .m<&cal_firstWeekday>("firstWeekday", "I@:").m<&cal_setFirstWeekday>("setFirstWeekday:", "v@:I")
     .m<&cal_minDays>("minimumDaysInFirstWeek", "I@:").m<&cal_setMinDays>("setMinimumDaysInFirstWeek:", "v@:I")
     .m<&cal_components>("components:fromDate:", "@@:I@")
     .m<&cal_dateFromComponents>("dateFromComponents:")
     .m<&cal_dateByAdding>("dateByAddingComponents:toDate:options:", "@@:@@I")
     .m<&cal_dateByAddingUnit>("dateByAddingUnit:value:toDate:options:", "@@:Ii@I")
     .m<&cal_difference>("components:fromDate:toDate:options:", "@@:I@@I")
     .m<&cal_ordinality>("ordinalityOfUnit:inUnit:forDate:", "I@:II@")
     .m<&cal_component>("component:fromDate:", "i@:I@")
     .m<&cal_startOfDay>("startOfDayForDate:")
     .m<&cal_isDateInToday>("isDateInToday:", "c@:@")
     .m<&cal_isSameDay>("isDate:inSameDayAsDate:", "c@:@@")
     .m<&cal_copy>("copyWithZone:").m<&cal_dealloc>("dealloc", "v@:");
}

} // namespace Hydrogen::Runtime::Foundation
