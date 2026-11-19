// HydrogenVM — Standard Java library classes (java.util.concurrent, java.io readers/writers/data,
// java.util.zip, java.util date/locale/tokenizer/bitset, java.text, java.net) for JNI and DEX activities.
#include "runtime/android/Jni.hpp"
#include "runtime/android/dex/GuestScheduler.hpp"
#include "runtime/android/dex/JniBridge.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

namespace Hydrogen::Runtime::Android {
namespace {

std::string str(Jni& j, JValue v) { return j.string(jobject(v)); }

jobject singleton(Jni& j, const std::string& cls) {
    const std::string key = "hvm.singleton." + cls;
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (auto it = j.staticFields.find(key); it != j.staticFields.end()) return jobject(it->second);
    const jobject h = j.newObject(cls);
    if (JObject* o = j.get(h)) o->permanent = true;
    j.staticFields[key] = h;
    return h;
}

std::string objectClass(Jni& j, jobject h) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(h)) return o->cls;
    return {};
}

// Helper to read all remaining bytes from any Java InputStream via JNI/DEX dispatch.
std::vector<uint8_t> readAllFromInputStream(Jni& j, Ctx& x, jobject stream) {
    std::vector<uint8_t> out;
    if (!stream) return out;
    const jobject chunk = j.newArray('B', 4096);
    const uint32_t mid = j.methodId("java/io/InputStream", "read", "([BII)I", false);
    for (int guard = 0; guard < 16384; ++guard) {
        const int32_t n = int32_t(j.invoke(x, mid, stream, {chunk, 0, 4096}));
        if (n <= 0) break;
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* arr = j.get(chunk)) {
            const size_t avail = std::min<size_t>(size_t(n), arr->data.size());
            out.insert(out.end(), arr->data.begin(), arr->data.begin() + avail);
        }
    }
    return out;
}

int32_t readByteFromInputStream(Jni& j, Ctx& x, jobject stream) {
    if (!stream) return -1;
    const uint32_t mid = j.methodId("java/io/InputStream", "read", "()I", false);
    return int32_t(j.invoke(x, mid, stream, {}));
}

void writeBytesToOutputStream(Jni& j, Ctx& x, jobject stream, const uint8_t* data, size_t len) {
    if (!stream || !len) return;
    const jobject arr = j.newArray('B', uint32_t(len));
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(arr)) std::memcpy(o->data.data(), data, len);
    }
    const uint32_t mid = j.methodId("java/io/OutputStream", "write", "([BII)V", false);
    j.invoke(x, mid, stream, {arr, 0, uint32_t(len)});
}

struct ReaderState {
    std::string text;
    size_t pos = 0;
};

struct WriterState {
    std::string buf;
    jobject wrapped = 0; // Optional OutputStream to flush to
};

struct InflaterState {
    z_stream zs{};
    bool initialized = false;
    bool finished = false;
    std::vector<uint8_t> input;
    ~InflaterState() {
        if (initialized) inflateEnd(&zs);
    }
};

struct DeflaterState {
    int level = Z_DEFAULT_COMPRESSION;
    bool ended = false;
};

struct ZipOutState {
    jobject outStream = 0;
    bool gzip = false;
    std::vector<uint8_t> raw;
    bool closed = false;
};

struct ZipInState {
    std::vector<uint8_t> data;
    size_t pos = 0;
};

struct TokenizerState {
    std::string text;
    std::string delim;
    size_t pos = 0;
};

std::vector<uint8_t> decompressZlib(const std::vector<uint8_t>& in, int windowBits) {
    std::vector<uint8_t> out;
    if (in.empty()) return out;
    z_stream zs{};
    if (inflateInit2(&zs, windowBits) != Z_OK) return out;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    uint8_t tmp[4096];
    int rc = Z_OK;
    while (rc == Z_OK) {
        zs.next_out = tmp;
        zs.avail_out = sizeof(tmp);
        rc = inflate(&zs, Z_NO_FLUSH);
        const size_t have = sizeof(tmp) - zs.avail_out;
        if (have) out.insert(out.end(), tmp, tmp + have);
        if (rc == Z_STREAM_END) break;
    }
    inflateEnd(&zs);
    return out;
}

std::vector<uint8_t> compressZlib(const std::vector<uint8_t>& in, int windowBits, int level) {
    std::vector<uint8_t> out;
    z_stream zs{};
    if (deflateInit2(&zs, level, Z_DEFLATED, windowBits, 8, Z_DEFAULT_STRATEGY) != Z_OK) return out;
    zs.next_in = const_cast<Bytef*>(in.data());
    zs.avail_in = uInt(in.size());
    uint8_t tmp[4096];
    int rc = Z_OK;
    while (rc != Z_STREAM_END) {
        zs.next_out = tmp;
        zs.avail_out = sizeof(tmp);
        rc = deflate(&zs, Z_FINISH);
        const size_t have = sizeof(tmp) - zs.avail_out;
        if (have) out.insert(out.end(), tmp, tmp + have);
        if (rc != Z_OK && rc != Z_STREAM_END) break;
    }
    deflateEnd(&zs);
    return out;
}

void scheduleRunnable(Jni& j, Ctx& x, jobject target) {
    if (!target) return;
    Process* proc = &x.proc;
    Dex::guestScheduler(*proc)([proc, target] {
        try {
            auto& jniEnv = jni(*proc);
            Ctx wx{*proc, proc->currentThread().cpu, proc->mem()};
            const std::string rcls = objectClass(jniEnv, target);
            JValue ignored = 0;
            if (!rcls.empty()) {
                Dex::invokeFromJni(jniEnv, wx, {rcls, "run", "()V", false}, target, {}, rcls, false, ignored);
            }
        } catch (...) {}
    });
}

} // namespace

void registerJavaStdlib(Jni& j) {
    // Superclass links
    j.superclasses["java/util/concurrent/ThreadPoolExecutor"] = "java/util/concurrent/AbstractExecutorService";
    j.superclasses["java/util/concurrent/LinkedBlockingQueue"] = "java/lang/Object";
    j.superclasses["java/util/concurrent/SynchronousQueue"] = "java/lang/Object";
    j.superclasses["java/io/BufferedReader"] = "java/io/Reader";
    j.superclasses["java/io/InputStreamReader"] = "java/io/Reader";
    j.superclasses["java/io/StringReader"] = "java/io/Reader";
    j.superclasses["java/io/BufferedWriter"] = "java/io/Writer";
    j.superclasses["java/io/OutputStreamWriter"] = "java/io/Writer";
    j.superclasses["java/io/StringWriter"] = "java/io/Writer";
    j.superclasses["java/io/PrintWriter"] = "java/io/Writer";
    j.superclasses["java/io/PrintStream"] = "java/io/OutputStream";
    j.superclasses["java/io/DataInputStream"] = "java/io/InputStream";
    j.superclasses["java/io/DataOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/util/zip/InflaterInputStream"] = "java/io/InputStream";
    j.superclasses["java/util/zip/GZIPInputStream"] = "java/util/zip/InflaterInputStream";
    j.superclasses["java/util/zip/DeflaterOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/util/zip/GZIPOutputStream"] = "java/util/zip/DeflaterOutputStream";
    j.superclasses["java/text/DecimalFormat"] = "java/text/NumberFormat";
    j.superclasses["java/text/NumberFormat"] = "java/text/Format";
    j.superclasses["java/text/SimpleDateFormat"] = "java/text/DateFormat";
    j.superclasses["java/text/DateFormat"] = "java/text/Format";
    j.superclasses["java/text/MessageFormat"] = "java/text/Format";
    j.superclasses["java/net/HttpURLConnection"] = "java/net/URLConnection";

    // java.util.concurrent.TimeUnit
    const std::string kTU = "java/util/concurrent/TimeUnit";
    const std::pair<const char*, uint64_t> kUnits[] = {
        {"NANOSECONDS", 1ULL},
        {"MICROSECONDS", 1000ULL},
        {"MILLISECONDS", 1000000ULL},
        {"SECONDS", 1000000000ULL},
        {"MINUTES", 60ULL * 1000000000ULL},
        {"HOURS", 3600ULL * 1000000000ULL},
        {"DAYS", 86400ULL * 1000000000ULL},
    };
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        for (const auto& [name, nanos] : kUnits) {
            const jobject u = j.newObject(kTU);
            if (JObject* o = j.get(u)) {
                o->permanent = true;
                o->fields["hvm.nanos"] = nanos;
            }
            j.staticFields[kTU + "." + name] = u;
        }
    }
    auto unitNanos = [](Jni& j, jobject u) -> uint64_t {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(u)) {
            if (auto it = o->fields.find("hvm.nanos"); it != o->fields.end() && it->second) return it->second;
        }
        return 1ULL;
    };
    j.method(kTU, "toNanos", "(J)J", [unitNanos](Jni& j, Ctx&, jobject self, const auto& a) {
        return JValue(uint64_t(int64_t(a[0]) * int64_t(unitNanos(j, self))));
    });
    j.method(kTU, "toMillis", "(J)J", [unitNanos](Jni& j, Ctx&, jobject self, const auto& a) {
        const int64_t ns = int64_t(a[0]) * int64_t(unitNanos(j, self));
        return JValue(uint64_t(ns / 1000000LL));
    });
    j.method(kTU, "convert", "(JLjava/util/concurrent/TimeUnit;)J", [unitNanos](Jni& j, Ctx&, jobject self, const auto& a) {
        const uint64_t src = unitNanos(j, jobject(a[1]));
        const uint64_t dst = unitNanos(j, self);
        if (src >= dst) return JValue(uint64_t(int64_t(a[0]) * int64_t(src / dst)));
        return JValue(uint64_t(int64_t(a[0]) / int64_t(dst / src)));
    });

    // Queues & ThreadPoolExecutor
    j.method("java/util/concurrent/LinkedBlockingQueue", "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/util/concurrent/SynchronousQueue", "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    const std::string kTPE = "java/util/concurrent/ThreadPoolExecutor";
    j.method(kTPE, "<init>", "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kTPE, "<init>", "(IIJLjava/util/concurrent/TimeUnit;Ljava/util/concurrent/BlockingQueue;Ljava/util/concurrent/ThreadFactory;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kTPE, "allowCoreThreadTimeOut", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kTPE, "execute", "(Ljava/lang/Runnable;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        scheduleRunnable(j, x, jobject(a[0]));
        return JValue(0);
    });
    j.method("java/util/concurrent/AbstractExecutorService", "submit", "(Ljava/lang/Runnable;)Ljava/util/concurrent/Future;",
             [](Jni& j, Ctx& x, jobject, const auto& a) {
                 scheduleRunnable(j, x, jobject(a[0]));
                 return JValue(j.newObject("java/util/concurrent/FutureTask"));
             });
    j.method("java/util/concurrent/Executor", "execute", "(Ljava/lang/Runnable;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        scheduleRunnable(j, x, jobject(a[0]));
        return JValue(0);
    });

    // java.util.concurrent.atomic.AtomicInteger
    const std::string kAI = "java/util/concurrent/atomic/AtomicInteger";
    j.method(kAI, "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["value"] = 0;
        return JValue(0);
    });
    j.method(kAI, "<init>", "(I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["value"] = uint32_t(a[0]);
        return JValue(0);
    });
    j.method(kAI, "get", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? uint32_t(o->fields["value"]) : 0u);
    });
    j.method(kAI, "set", "(I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["value"] = uint32_t(a[0]);
        return JValue(0);
    });
    j.method(kAI, "getAndIncrement", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return JValue(0);
        const int32_t prev = int32_t(o->fields["value"]);
        o->fields["value"] = uint32_t(prev + 1);
        return JValue(uint32_t(prev));
    });
    j.method(kAI, "incrementAndGet", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return JValue(0);
        const int32_t next = int32_t(o->fields["value"]) + 1;
        o->fields["value"] = uint32_t(next);
        return JValue(uint32_t(next));
    });
    j.method(kAI, "compareAndSet", "(II)Z", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o || uint32_t(o->fields["value"]) != uint32_t(a[0])) return JValue(0);
        o->fields["value"] = uint32_t(a[1]);
        return JValue(1);
    });

    // java.util.concurrent.locks.LockSupport
    const std::string kLS = "java/util/concurrent/locks/LockSupport";
    j.method(kLS, "park", "(Ljava/lang/Object;)V", [](Jni&, Ctx&, jobject, const auto&) {
        std::this_thread::yield();
        return JValue(0);
    });
    j.method(kLS, "parkNanos", "(Ljava/lang/Object;J)V", [](Jni&, Ctx&, jobject, const auto& a) {
        const int64_t ns = int64_t(a[1]);
        if (ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(std::min<int64_t>(ns, 10000000LL)));
        return JValue(0);
    });
    j.method(kLS, "unpark", "(Ljava/lang/Thread;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.util.Locale
    const std::string kLoc = "java/util/Locale";
    auto makeLocale = [kLoc](Jni& j, const std::string& lang, const std::string& country, const std::string& variant) {
        const jobject loc = j.newObject(kLoc);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(loc)) {
            o->fields["hvm.lang"] = j.newString(lang);
            o->fields["hvm.country"] = j.newString(country);
            o->fields["hvm.variant"] = j.newString(variant);
        }
        return loc;
    };
    {
        const jobject us = makeLocale(j, "en", "US", "");
        const jobject en = makeLocale(j, "en", "", "");
        const jobject root = makeLocale(j, "", "", "");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        for (jobject h : {us, en, root}) if (JObject* o = j.get(h)) o->permanent = true;
        j.staticFields[kLoc + ".US"] = us;
        j.staticFields[kLoc + ".ENGLISH"] = en;
        j.staticFields[kLoc + ".ROOT"] = root;
        j.staticFields[kLoc + ".DEFAULT"] = us;
    }
    j.method(kLoc, "<init>", "(Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            o->fields["hvm.lang"] = a[0];
            o->fields["hvm.country"] = j.newString("");
            o->fields["hvm.variant"] = j.newString("");
        }
        return JValue(0);
    });
    j.method(kLoc, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            o->fields["hvm.lang"] = a[0];
            o->fields["hvm.country"] = a[1];
            o->fields["hvm.variant"] = j.newString("");
        }
        return JValue(0);
    });
    j.method(kLoc, "getDefault", "()Ljava/util/Locale;", [kLoc](Jni& j, Ctx&, jobject, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        return JValue(j.staticFields[kLoc + ".DEFAULT"]);
    });
    j.method(kLoc, "getLanguage", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o && o->fields["hvm.lang"] ? o->fields["hvm.lang"] : j.newString("en"));
    });
    j.method(kLoc, "getCountry", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o && o->fields["hvm.country"] ? o->fields["hvm.country"] : j.newString(""));
    });
    j.method(kLoc, "getVariant", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o && o->fields["hvm.variant"] ? o->fields["hvm.variant"] : j.newString(""));
    });
    j.method(kLoc, "toString", "()Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::string l, c, v;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) {
                l = str(j, o->fields["hvm.lang"]);
                c = str(j, o->fields["hvm.country"]);
                v = str(j, o->fields["hvm.variant"]);
            }
        }
        std::string s = l;
        if (!c.empty() || !v.empty()) s += "_" + c;
        if (!v.empty()) s += "_" + v;
        return JValue(j.newString(s));
    });
    j.method(kLoc, "equals", "(Ljava/lang/Object;)Z", [](Jni& j, Ctx&, jobject self, const auto& a) {
        if (self == jobject(a[0])) return JValue(1);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* A = j.get(self), *B = j.get(jobject(a[0]));
        if (!A || !B || B->cls != "java/util/Locale") return JValue(0);
        return JValue(str(j, A->fields["hvm.lang"]) == str(j, B->fields["hvm.lang"]) &&
                      str(j, A->fields["hvm.country"]) == str(j, B->fields["hvm.country"]) &&
                      str(j, A->fields["hvm.variant"]) == str(j, B->fields["hvm.variant"]) ? 1 : 0);
    });

    // java.util.TimeZone, Date, Calendar
    j.method("java/util/TimeZone", "getTimeZone", "(Ljava/lang/String;)Ljava/util/TimeZone;", [](Jni& j, Ctx&, jobject, const auto& a) {
        const jobject tz = j.newObject("java/util/TimeZone");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(tz)) o->fields["hvm.id"] = a[0];
        return JValue(tz);
    });
    j.method("java/util/TimeZone", "getDefault", "()Ljava/util/TimeZone;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "java/util/TimeZone"));
    });
    auto nowMs = []() -> int64_t {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    };
    const std::string kDate = "java/util/Date";
    j.method(kDate, "<init>", "()V", [nowMs](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.ms"] = uint64_t(nowMs());
        return JValue(0);
    });
    j.method(kDate, "<init>", "(J)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.ms"] = a[0];
        return JValue(0);
    });
    j.method(kDate, "getTime", "()J", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? o->fields["hvm.ms"] : 0ULL);
    });
    j.method(kDate, "setTime", "(J)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.ms"] = a[0];
        return JValue(0);
    });
    j.method(kDate, "after", "(Ljava/util/Date;)Z", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* A = j.get(self), *B = j.get(jobject(a[0]));
        return JValue(A && B && int64_t(A->fields["hvm.ms"]) > int64_t(B->fields["hvm.ms"]) ? 1 : 0);
    });
    const std::string kCal = "java/util/Calendar";
    j.method(kCal, "getInstance", "()Ljava/util/Calendar;", [kCal, nowMs](Jni& j, Ctx&, jobject, const auto&) {
        const jobject cal = j.newObject(kCal);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(cal)) o->fields["hvm.ms"] = uint64_t(nowMs());
        return JValue(cal);
    });
    j.method(kCal, "setTime", "(Ljava/util/Date;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* cal = j.get(self), *d = j.get(jobject(a[0]));
        if (cal && d) cal->fields["hvm.ms"] = d->fields["hvm.ms"];
        return JValue(0);
    });
    j.method(kCal, "get", "(I)I", [](Jni& j, Ctx&, jobject self, const auto& a) {
        int64_t ms = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) ms = int64_t(o->fields["hvm.ms"]);
        }
        const std::time_t tt = std::time_t(ms / 1000);
        std::tm tm{};
        gmtime_r(&tt, &tm);
        switch (int32_t(a[0])) {
            case 1: return JValue(uint32_t(tm.tm_year + 1900)); // YEAR
            case 2: return JValue(uint32_t(tm.tm_mon));         // MONTH (0-based)
            case 5: return JValue(uint32_t(tm.tm_mday));        // DAY_OF_MONTH
            case 6: return JValue(uint32_t(tm.tm_yday + 1));    // DAY_OF_YEAR
            case 7: return JValue(uint32_t(tm.tm_wday + 1));    // DAY_OF_WEEK (1=Sun)
            case 11: return JValue(uint32_t(tm.tm_hour));       // HOUR_OF_DAY
            case 12: return JValue(uint32_t(tm.tm_min));        // MINUTE
            case 13: return JValue(uint32_t(tm.tm_sec));        // SECOND
            default: return JValue(0);
        }
    });

    // java.util.StringTokenizer
    const std::string kST = "java/util/StringTokenizer";
    auto tokOf = [](Jni& j, jobject self) -> std::shared_ptr<TokenizerState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<TokenizerState>>(&o->host)) return *sp;
        return nullptr;
    };
    j.method(kST, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = std::make_shared<TokenizerState>();
        st->text = str(j, a[0]);
        st->delim = str(j, a[1]);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
        return JValue(0);
    });
    j.method(kST, "hasMoreTokens", "()Z", [tokOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = tokOf(j, self);
        if (!st) return JValue(0);
        while (st->pos < st->text.size() && st->delim.find(st->text[st->pos]) != std::string::npos) ++st->pos;
        return JValue(st->pos < st->text.size() ? 1 : 0);
    });
    j.method(kST, "nextToken", "()Ljava/lang/String;", [tokOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = tokOf(j, self);
        if (!st) return JValue(j.newString(""));
        while (st->pos < st->text.size() && st->delim.find(st->text[st->pos]) != std::string::npos) ++st->pos;
        const size_t start = st->pos;
        while (st->pos < st->text.size() && st->delim.find(st->text[st->pos]) == std::string::npos) ++st->pos;
        return JValue(j.newString(st->text.substr(start, st->pos - start)));
    });

    // java.util.BitSet
    const std::string kBS = "java/util/BitSet";
    j.method(kBS, "valueOf", "([B)Ljava/util/BitSet;", [kBS](Jni& j, Ctx&, jobject, const auto& a) {
        const jobject bs = j.newObject(kBS);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* src = j.get(jobject(a[0])), *dst = j.get(bs);
        if (src && dst) dst->data = src->data;
        return JValue(bs);
    });
    j.method(kBS, "get", "(I)Z", [](Jni& j, Ctx&, jobject self, const auto& a) {
        const int32_t idx = int32_t(a[0]);
        if (idx < 0) return JValue(0);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return JValue(0);
        const size_t byteIdx = size_t(idx) / 8;
        if (byteIdx >= o->data.size()) return JValue(0);
        return JValue((o->data[byteIdx] >> (idx & 7)) & 1);
    });

    // java.io.Reader / InputStreamReader / BufferedReader / StringReader
    auto readerOf = [](Jni& j, jobject self) -> std::shared_ptr<ReaderState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<ReaderState>>(&o->host)) return *sp;
        return nullptr;
    };
    j.method("java/io/StringReader", "<init>", "(Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = std::make_shared<ReaderState>();
        st->text = str(j, a[0]);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
        return JValue(0);
    });
    auto initIsr = [](Jni& j, Ctx& x, jobject self, jobject inStream) {
        auto bytes = readAllFromInputStream(j, x, inStream);
        auto st = std::make_shared<ReaderState>();
        st->text.assign(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
    };
    j.method("java/io/InputStreamReader", "<init>", "(Ljava/io/InputStream;)V", [initIsr](Jni& j, Ctx& x, jobject self, const auto& a) {
        initIsr(j, x, self, jobject(a[0]));
        return JValue(0);
    });
    j.method("java/io/InputStreamReader", "<init>", "(Ljava/io/InputStream;Ljava/lang/String;)V", [initIsr](Jni& j, Ctx& x, jobject self, const auto& a) {
        initIsr(j, x, self, jobject(a[0]));
        return JValue(0);
    });
    auto initBr = [readerOf](Jni& j, jobject self, jobject inner) {
        auto st = readerOf(j, inner);
        if (!st) st = std::make_shared<ReaderState>();
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
    };
    j.method("java/io/BufferedReader", "<init>", "(Ljava/io/Reader;)V", [initBr](Jni& j, Ctx&, jobject self, const auto& a) {
        initBr(j, self, jobject(a[0]));
        return JValue(0);
    });
    j.method("java/io/BufferedReader", "<init>", "(Ljava/io/Reader;I)V", [initBr](Jni& j, Ctx&, jobject self, const auto& a) {
        initBr(j, self, jobject(a[0]));
        return JValue(0);
    });
    j.method("java/io/BufferedReader", "readLine", "()Ljava/lang/String;", [readerOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = readerOf(j, self);
        if (!st || st->pos >= st->text.size()) return JValue(0);
        const size_t start = st->pos;
        while (st->pos < st->text.size() && st->text[st->pos] != '\n' && st->text[st->pos] != '\r') ++st->pos;
        const std::string line = st->text.substr(start, st->pos - start);
        if (st->pos < st->text.size() && st->text[st->pos] == '\r') ++st->pos;
        if (st->pos < st->text.size() && st->text[st->pos] == '\n') ++st->pos;
        return JValue(j.newString(line));
    });
    j.method("java/io/Reader", "markSupported", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/Reader", "read", "()I", [readerOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = readerOf(j, self);
        if (!st || st->pos >= st->text.size()) return JValue(uint32_t(-1));
        return JValue(uint32_t(uint8_t(st->text[st->pos++])));
    });
    j.method("java/io/Reader", "read", "([CII)I", [readerOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = readerOf(j, self);
        if (!st || st->pos >= st->text.size()) return JValue(uint32_t(-1));
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* arr = j.get(jobject(a[0]));
        if (!arr) return JValue(uint32_t(-1));
        const size_t cap = arr->data.size() / 2;
        if (size_t(off) + len > cap) return JValue(uint32_t(-1));
        const size_t n = std::min<size_t>(len, st->text.size() - st->pos);
        for (size_t i = 0; i < n; ++i) {
            const uint16_t ch = uint8_t(st->text[st->pos++]);
            std::memcpy(arr->data.data() + (off + i) * 2, &ch, 2);
        }
        return JValue(uint32_t(n));
    });
    j.method("java/io/Reader", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.io.Writer / OutputStreamWriter / BufferedWriter / StringWriter / PrintWriter / PrintStream
    auto writerOf = [](Jni& j, jobject self) -> std::shared_ptr<WriterState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<WriterState>>(&o->host)) return *sp;
        return nullptr;
    };
    j.method("java/io/StringWriter", "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = std::make_shared<WriterState>();
        return JValue(0);
    });
    j.method("java/io/StringWriter", "toString", "()Ljava/lang/String;", [writerOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = writerOf(j, self);
        return JValue(j.newString(st ? st->buf : ""));
    });
    j.method("java/io/OutputStreamWriter", "<init>", "(Ljava/io/OutputStream;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = std::make_shared<WriterState>();
        st->wrapped = jobject(a[0]);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
        return JValue(0);
    });
    for (const char* wcls : {"java/io/BufferedWriter", "java/io/PrintWriter"}) {
        j.method(wcls, "<init>", "(Ljava/io/Writer;)V", [writerOf](Jni& j, Ctx&, jobject self, const auto& a) {
            auto st = writerOf(j, jobject(a[0]));
            if (!st) st = std::make_shared<WriterState>();
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) o->host = st;
            return JValue(0);
        });
    }
    auto flushWriter = [writerOf](Jni& j, Ctx& x, jobject self) {
        auto st = writerOf(j, self);
        if (st && st->wrapped && !st->buf.empty()) {
            writeBytesToOutputStream(j, x, st->wrapped, reinterpret_cast<const uint8_t*>(st->buf.data()), st->buf.size());
            st->buf.clear();
        }
    };
    j.method("java/io/Writer", "write", "(I)V", [writerOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = writerOf(j, self)) st->buf.push_back(char(a[0] & 0xff));
        return JValue(0);
    });
    j.method("java/io/Writer", "write", "(Ljava/lang/String;)V", [writerOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = writerOf(j, self)) st->buf += str(j, a[0]);
        return JValue(0);
    });
    j.method("java/io/Writer", "write", "(Ljava/lang/String;II)V", [writerOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = writerOf(j, self)) {
            const std::string s = str(j, a[0]);
            const size_t off = size_t(std::max<int32_t>(0, int32_t(a[1])));
            const size_t len = size_t(std::max<int32_t>(0, int32_t(a[2])));
            if (off < s.size()) st->buf += s.substr(off, len);
        }
        return JValue(0);
    });
    j.method("java/io/Writer", "flush", "()V", [flushWriter](Jni& j, Ctx& x, jobject self, const auto&) {
        flushWriter(j, x, self);
        return JValue(0);
    });
    j.method("java/io/Writer", "close", "()V", [flushWriter, writerOf](Jni& j, Ctx& x, jobject self, const auto&) {
        flushWriter(j, x, self);
        if (auto st = writerOf(j, self); st && st->wrapped) {
            j.invoke(x, j.methodId("java/io/OutputStream", "close", "()V", false), st->wrapped, {});
        }
        return JValue(0);
    });
    j.method("java/io/PrintStream", "println", "(Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/PrintStream", "println", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/PrintStream", "println", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/PrintStream", "print", "(Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.io.DataInputStream & DataOutputStream (big-endian per java.io.DataInput/DataOutput spec)
    const std::string kDIS = "java/io/DataInputStream";
    j.method(kDIS, "<init>", "(Ljava/io/InputStream;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.in"] = a[0];
        return JValue(0);
    });
    auto disInner = [](Jni& j, jobject self) -> jobject {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return o ? jobject(o->fields["hvm.in"]) : 0;
    };
    j.method(kDIS, "read", "()I", [disInner](Jni& j, Ctx& x, jobject self, const auto&) {
        return JValue(uint32_t(readByteFromInputStream(j, x, disInner(j, self))));
    });
    j.method(kDIS, "read", "([B)I", [disInner](Jni& j, Ctx& x, jobject self, const auto& a) {
        const jobject in = disInner(j, self);
        uint32_t len = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* arr = j.get(jobject(a[0]))) len = uint32_t(arr->data.size());
        }
        return j.invoke(x, j.methodId("java/io/InputStream", "read", "([BII)I", false), in, {a[0], 0, len});
    });
    j.method(kDIS, "read", "([BII)I", [disInner](Jni& j, Ctx& x, jobject self, const auto& a) {
        return j.invoke(x, j.methodId("java/io/InputStream", "read", "([BII)I", false), disInner(j, self), a);
    });
    j.method(kDIS, "readInt", "()I", [disInner](Jni& j, Ctx& x, jobject self, const auto&) {
        const jobject in = disInner(j, self);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | uint32_t(readByteFromInputStream(j, x, in) & 0xff);
        return JValue(v);
    });
    j.method(kDIS, "readLong", "()J", [disInner](Jni& j, Ctx& x, jobject self, const auto&) {
        const jobject in = disInner(j, self);
        uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | uint64_t(readByteFromInputStream(j, x, in) & 0xff);
        return JValue(v);
    });
    j.method(kDIS, "close", "()V", [disInner](Jni& j, Ctx& x, jobject self, const auto&) {
        if (const jobject in = disInner(j, self))
            j.invoke(x, j.methodId("java/io/InputStream", "close", "()V", false), in, {});
        return JValue(0);
    });

    const std::string kDOS = "java/io/DataOutputStream";
    j.method(kDOS, "<init>", "(Ljava/io/OutputStream;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.out"] = a[0];
        return JValue(0);
    });
    auto dosInner = [](Jni& j, jobject self) -> jobject {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return o ? jobject(o->fields["hvm.out"]) : 0;
    };
    j.method(kDOS, "writeInt", "(I)V", [dosInner](Jni& j, Ctx& x, jobject self, const auto& a) {
        const uint32_t v = uint32_t(a[0]);
        const uint8_t buf[4] = {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
        writeBytesToOutputStream(j, x, dosInner(j, self), buf, 4);
        return JValue(0);
    });
    j.method(kDOS, "writeLong", "(J)V", [dosInner](Jni& j, Ctx& x, jobject self, const auto& a) {
        const uint64_t v = a[0];
        const uint8_t buf[8] = {
            uint8_t(v >> 56), uint8_t(v >> 48), uint8_t(v >> 40), uint8_t(v >> 32),
            uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v),
        };
        writeBytesToOutputStream(j, x, dosInner(j, self), buf, 8);
        return JValue(0);
    });
    j.method(kDOS, "close", "()V", [dosInner](Jni& j, Ctx& x, jobject self, const auto&) {
        if (const jobject out = dosInner(j, self))
            j.invoke(x, j.methodId("java/io/OutputStream", "close", "()V", false), out, {});
        return JValue(0);
    });

    // java.util.zip.CRC32, Inflater, Deflater, InflaterInputStream, GZIPInputStream, DeflaterOutputStream, GZIPOutputStream
    const std::string kCRC = "java/util/zip/CRC32";
    j.method(kCRC, "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.crc"] = uint64_t(::crc32(0L, Z_NULL, 0));
        return JValue(0);
    });
    j.method(kCRC, "update", "([BII)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self), *arr = j.get(jobject(a[0]));
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        if (o && arr && size_t(off) + len <= arr->data.size()) {
            o->fields["hvm.crc"] = uint64_t(::crc32(uLong(o->fields["hvm.crc"]), arr->data.data() + off, uInt(len)));
        }
        return JValue(0);
    });
    j.method(kCRC, "getValue", "()J", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? (o->fields["hvm.crc"] & 0xffffffffULL) : 0ULL);
    });

    const std::string kInf = "java/util/zip/Inflater";
    auto infOf = [](Jni& j, jobject self) -> std::shared_ptr<InflaterState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<InflaterState>>(&o->host)) return *sp;
        return nullptr;
    };
    j.method(kInf, "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = std::make_shared<InflaterState>();
        st->initialized = (inflateInit(&st->zs) == Z_OK);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
        return JValue(0);
    });
    j.method(kInf, "setInput", "([BII)V", [infOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = infOf(j, self);
        if (!st) return JValue(0);
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* arr = j.get(jobject(a[0])); arr && size_t(off) + len <= arr->data.size()) {
            st->input.assign(arr->data.begin() + off, arr->data.begin() + off + len);
            st->zs.next_in = st->input.data();
            st->zs.avail_in = uInt(st->input.size());
        }
        return JValue(0);
    });
    j.method(kInf, "inflate", "([BII)I", [infOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = infOf(j, self);
        if (!st || !st->initialized || st->finished) return JValue(0);
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* arr = j.get(jobject(a[0]));
        if (!arr || size_t(off) + len > arr->data.size()) return JValue(0);
        st->zs.next_out = arr->data.data() + off;
        st->zs.avail_out = uInt(len);
        const int rc = inflate(&st->zs, Z_NO_FLUSH);
        if (rc == Z_STREAM_END) st->finished = true;
        return JValue(uint32_t(len - st->zs.avail_out));
    });
    j.method(kInf, "finished", "()Z", [infOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = infOf(j, self);
        return JValue(st && st->finished ? 1 : 0);
    });
    j.method(kInf, "needsDictionary", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kInf, "end", "()V", [infOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = infOf(j, self); st && st->initialized) {
            inflateEnd(&st->zs);
            st->initialized = false;
        }
        return JValue(0);
    });

    const std::string kDef = "java/util/zip/Deflater";
    j.method(kDef, "<init>", "(I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = std::make_shared<DeflaterState>();
        st->level = int32_t(a[0]);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
        return JValue(0);
    });
    j.method(kDef, "end", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    auto zipInOf = [](Jni& j, jobject self) -> std::shared_ptr<ZipInState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<ZipInState>>(&o->host)) return *sp;
        return nullptr;
    };
    auto initZipIn = [](Jni& j, Ctx& x, jobject self, jobject inStream, int windowBits) {
        auto raw = readAllFromInputStream(j, x, inStream);
        auto st = std::make_shared<ZipInState>();
        st->data = decompressZlib(raw, windowBits);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = st;
    };
    j.method("java/util/zip/InflaterInputStream", "<init>", "(Ljava/io/InputStream;)V", [initZipIn](Jni& j, Ctx& x, jobject self, const auto& a) {
        initZipIn(j, x, self, jobject(a[0]), 15);
        return JValue(0);
    });
    j.method("java/util/zip/GZIPInputStream", "<init>", "(Ljava/io/InputStream;)V", [initZipIn](Jni& j, Ctx& x, jobject self, const auto& a) {
        initZipIn(j, x, self, jobject(a[0]), 15 + 32);
        return JValue(0);
    });
    j.method("java/util/zip/GZIPInputStream", "<init>", "(Ljava/io/InputStream;I)V", [initZipIn](Jni& j, Ctx& x, jobject self, const auto& a) {
        initZipIn(j, x, self, jobject(a[0]), 15 + 32);
        return JValue(0);
    });
    j.method("java/util/zip/InflaterInputStream", "read", "()I", [zipInOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto st = zipInOf(j, self);
        if (!st || st->pos >= st->data.size()) return JValue(uint32_t(-1));
        return JValue(uint32_t(st->data[st->pos++]));
    });
    j.method("java/util/zip/InflaterInputStream", "read", "([BII)I", [zipInOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = zipInOf(j, self);
        if (!st || st->pos >= st->data.size()) return JValue(uint32_t(-1));
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* arr = j.get(jobject(a[0]));
        if (!arr || size_t(off) + len > arr->data.size()) return JValue(uint32_t(-1));
        const size_t n = std::min<size_t>(len, st->data.size() - st->pos);
        if (n) std::memcpy(arr->data.data() + off, st->data.data() + st->pos, n);
        st->pos += n;
        return JValue(uint32_t(n));
    });
    j.method("java/util/zip/InflaterInputStream", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    auto zipOutOf = [](Jni& j, jobject self) -> std::shared_ptr<ZipOutState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* sp = std::any_cast<std::shared_ptr<ZipOutState>>(&o->host)) return *sp;
        return nullptr;
    };
    j.method("java/util/zip/DeflaterOutputStream", "<init>", "(Ljava/io/OutputStream;Ljava/util/zip/Deflater;)V",
             [](Jni& j, Ctx&, jobject self, const auto& a) {
                 auto st = std::make_shared<ZipOutState>();
                 st->outStream = jobject(a[0]);
                 st->gzip = false;
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* o = j.get(self)) o->host = st;
                 return JValue(0);
             });
    j.method("java/util/zip/GZIPOutputStream", "<init>", "(Ljava/io/OutputStream;I)V",
             [](Jni& j, Ctx&, jobject self, const auto& a) {
                 auto st = std::make_shared<ZipOutState>();
                 st->outStream = jobject(a[0]);
                 st->gzip = true;
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* o = j.get(self)) o->host = st;
                 return JValue(0);
             });
    j.method("java/util/zip/DeflaterOutputStream", "write", "([B)V", [zipOutOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = zipOutOf(j, self);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (st) if (JObject* arr = j.get(jobject(a[0]))) st->raw.insert(st->raw.end(), arr->data.begin(), arr->data.end());
        return JValue(0);
    });
    j.method("java/util/zip/DeflaterOutputStream", "write", "([BII)V", [zipOutOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto st = zipOutOf(j, self);
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (st) if (JObject* arr = j.get(jobject(a[0])); arr && size_t(off) + len <= arr->data.size())
            st->raw.insert(st->raw.end(), arr->data.begin() + off, arr->data.begin() + off + len);
        return JValue(0);
    });
    j.method("java/util/zip/DeflaterOutputStream", "close", "()V", [zipOutOf](Jni& j, Ctx& x, jobject self, const auto&) {
        auto st = zipOutOf(j, self);
        if (st && !st->closed) {
            st->closed = true;
            auto compressed = compressZlib(st->raw, st->gzip ? (15 + 16) : 15, Z_DEFAULT_COMPRESSION);
            if (st->outStream) {
                writeBytesToOutputStream(j, x, st->outStream, compressed.data(), compressed.size());
                j.invoke(x, j.methodId("java/io/OutputStream", "close", "()V", false), st->outStream, {});
            }
        }
        return JValue(0);
    });

    // java.text.* formatting helpers
    j.method("java/text/DecimalFormatSymbols", "getInstance", "(Ljava/util/Locale;)Ljava/text/DecimalFormatSymbols;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newObject("java/text/DecimalFormatSymbols")); });
    j.method("java/text/DecimalFormat", "<init>", "(Ljava/lang/String;Ljava/text/DecimalFormatSymbols;)V",
             [](Jni& j, Ctx&, jobject self, const auto& a) {
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* o = j.get(self)) o->fields["hvm.pat"] = a[0];
                 return JValue(0);
             });
    j.method("java/text/NumberFormat", "getInstance", "(Ljava/util/Locale;)Ljava/text/NumberFormat;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(j.newObject("java/text/NumberFormat")); });
    j.method("java/text/NumberFormat", "format", "(J)Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto& a) {
        return JValue(j.newString(std::to_string(int64_t(a[0]))));
    });
    j.method("java/text/NumberFormat", "format", "(D)Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto& a) {
        double v = 0;
        std::memcpy(&v, &a[0], sizeof(v));
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.2f", v);
        return JValue(j.newString(buf));
    });
    j.method("java/text/SimpleDateFormat", "<init>", "(Ljava/lang/String;Ljava/util/Locale;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/text/DateFormat", "setTimeZone", "(Ljava/util/TimeZone;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/text/DateFormat", "format", "(Ljava/util/Date;)Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newString("1970-01-01"));
    });
    j.method("java/text/DateFormat", "parse", "(Ljava/lang/String;)Ljava/util/Date;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/util/Date"));
    });
    j.method("java/text/MessageFormat", "<init>", "(Ljava/lang/String;Ljava/util/Locale;)V",
             [](Jni& j, Ctx&, jobject self, const auto& a) {
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* o = j.get(self)) o->fields["hvm.pat"] = a[0];
                 return JValue(0);
             });
    j.method("java/text/Format", "format", "(Ljava/lang/Object;)Ljava/lang/String;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (o && o->fields["hvm.pat"]) return JValue(o->fields["hvm.pat"]);
        return JValue(j.newString(""));
    });

    // java.net.* offline-safe URLConnection
    j.method("java/net/URL", "<init>", "(Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.url"] = a[0];
        return JValue(0);
    });
    j.method("java/net/URL", "openConnection", "()Ljava/net/URLConnection;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/net/HttpURLConnection"));
    });
    j.method("java/net/URLConnection", "addRequestProperty", "(Ljava/lang/String;Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "setConnectTimeout", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "setReadTimeout", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "setDoInput", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "setDoOutput", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "connect", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/URLConnection", "getInputStream", "()Ljava/io/InputStream;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/io/ByteArrayInputStream"));
    });
    j.method("java/net/HttpURLConnection", "setFollowRedirects", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/HttpURLConnection", "setRequestMethod", "(Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/net/HttpURLConnection", "getResponseCode", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(503); });
    j.method("java/net/HttpURLConnection", "getErrorStream", "()Ljava/io/InputStream;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/io/ByteArrayInputStream"));
    });
    j.method("java/net/HttpURLConnection", "disconnect", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.lang.Class and java.lang.reflect.*
    j.superclasses["java/lang/reflect/Constructor"] = "java/lang/reflect/AccessibleObject";
    j.superclasses["java/lang/reflect/Method"] = "java/lang/reflect/AccessibleObject";
    j.superclasses["java/lang/reflect/Field"] = "java/lang/reflect/AccessibleObject";

    auto classTarget = [](Jni& j, jobject clsObj) -> std::string {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(clsObj)) return o->cls;
        return {};
    };
    auto toDescriptor = [](const std::string& internal) -> std::string {
        if (internal.empty()) return "Ljava/lang/Object;";
        if (internal[0] == '[') return internal;
        if (internal == "boolean" || internal == "Z") return "Z";
        if (internal == "byte" || internal == "B") return "B";
        if (internal == "char" || internal == "C") return "C";
        if (internal == "short" || internal == "S") return "S";
        if (internal == "int" || internal == "I") return "I";
        if (internal == "long" || internal == "J") return "J";
        if (internal == "float" || internal == "F") return "F";
        if (internal == "double" || internal == "D") return "D";
        if (internal == "void" || internal == "V") return "V";
        return "L" + internal + ";";
    };
    const std::string kCls = "java/lang/Class";
    j.method(kCls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::string nm = str(j, a[0]);
        std::replace(nm.begin(), nm.end(), '.', '/');
        return JValue(j.classFor(nm));
    });
    j.method(kCls, "getSimpleName", "()Ljava/lang/String;", [classTarget](Jni& j, Ctx&, jobject self, const auto&) {
        std::string nm = classTarget(j, self);
        const auto dollar = nm.rfind('$');
        const auto slash = nm.rfind('/');
        const auto cut = (dollar != std::string::npos && (slash == std::string::npos || dollar > slash)) ? dollar : slash;
        if (cut != std::string::npos) nm = nm.substr(cut + 1);
        return JValue(j.newString(nm));
    });
    j.method(kCls, "getSuperclass", "()Ljava/lang/Class;", [classTarget](Jni& j, Ctx& x, jobject self, const auto&) {
        const std::string nm = classTarget(j, self);
        if (nm.empty() || nm == "java/lang/Object") return JValue(0);
        std::string sup;
        uint32_t flags = 0;
        if (Dex::classInfoFromJni(x.proc, nm, sup, flags)) {
            if (flags & 0x200) return JValue(0); // interface
            return JValue(sup.empty() ? 0 : j.classFor(sup));
        }
        const std::string jsup = j.superOf(nm);
        return JValue(jsup.empty() ? j.classFor("java/lang/Object") : j.classFor(jsup));
    });
    j.method(kCls, "isArray", "()Z", [classTarget](Jni& j, Ctx&, jobject self, const auto&) {
        const std::string nm = classTarget(j, self);
        return JValue(!nm.empty() && nm[0] == '[' ? 1 : 0);
    });
    j.method(kCls, "getComponentType", "()Ljava/lang/Class;", [classTarget](Jni& j, Ctx&, jobject self, const auto&) {
        const std::string nm = classTarget(j, self);
        if (nm.size() < 2 || nm[0] != '[') return JValue(0);
        std::string comp = nm.substr(1);
        if (comp.size() > 2 && comp.front() == 'L' && comp.back() == ';') comp = comp.substr(1, comp.size() - 2);
        return JValue(j.classFor(comp));
    });
    j.method(kCls, "isInterface", "()Z", [classTarget](Jni& j, Ctx& x, jobject self, const auto&) {
        std::string sup;
        uint32_t flags = 0;
        if (Dex::classInfoFromJni(x.proc, classTarget(j, self), sup, flags)) return JValue((flags & 0x200) ? 1 : 0);
        return JValue(0);
    });
    j.method(kCls, "isMemberClass", "()Z", [classTarget](Jni& j, Ctx&, jobject self, const auto&) {
        return JValue(classTarget(j, self).find('$') != std::string::npos ? 1 : 0);
    });
    j.method(kCls, "getModifiers", "()I", [classTarget](Jni& j, Ctx& x, jobject self, const auto&) {
        std::string sup;
        uint32_t flags = 1; // ACC_PUBLIC
        Dex::classInfoFromJni(x.proc, classTarget(j, self), sup, flags);
        return JValue(flags);
    });
    j.method(kCls, "isAssignableFrom", "(Ljava/lang/Class;)Z", [classTarget](Jni& j, Ctx& x, jobject self, const auto& a) {
        const std::string to = classTarget(j, self);
        const std::string from = classTarget(j, jobject(a[0]));
        if (from.empty() || to.empty()) return JValue(0);
        if (from == to || to == "java/lang/Object") return JValue(1);
        return JValue((Dex::isAssignableFromJni(x.proc, from, to) || j.isSubclass(from, to)) ? 1 : 0);
    });
    j.method(kCls, "isInstance", "(Ljava/lang/Object;)Z", [classTarget](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (!a[0]) return JValue(0);
        const std::string to = classTarget(j, self);
        const std::string from = objectClass(j, jobject(a[0]));
        return JValue((Dex::isAssignableFromJni(x.proc, from, to) || j.isSubclass(from, to)) ? 1 : 0);
    });
    j.method(kCls, "newInstance", "()Ljava/lang/Object;", [classTarget](Jni& j, Ctx& x, jobject self, const auto&) {
        const std::string cls = classTarget(j, self);
        jobject out = 0;
        if (Dex::instantiateFromJni(j, x, cls, "()V", {}, out)) return JValue(out);
        out = j.newObject(cls);
        const uint32_t mid = j.methodId(cls, "<init>", "()V", false);
        j.invoke(x, mid, out, {});
        return JValue(out);
    });
    auto makeCtor = [classTarget, toDescriptor](Jni& j, jobject self, jobject paramTypesArr) -> jobject {
        const std::string cls = classTarget(j, self);
        std::string sig = "(";
        if (paramTypesArr) {
            std::vector<jobject> ptypes;
            {
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* arr = j.get(paramTypesArr)) ptypes = arr->refs;
            }
            for (jobject pt : ptypes) sig += toDescriptor(classTarget(j, pt));
        }
        sig += ")V";
        const jobject ctor = j.newObject("java/lang/reflect/Constructor");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(ctor)) {
            o->fields["hvm.declaring"] = self;
            o->fields["hvm.sig"] = j.newString(sig);
            o->fields["hvm.accessible"] = 1;
        }
        return ctor;
    };
    j.method(kCls, "getConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", [makeCtor](Jni& j, Ctx&, jobject self, const auto& a) {
        return JValue(makeCtor(j, self, jobject(a[0])));
    });
    j.method(kCls, "getDeclaredConstructor", "([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;", [makeCtor](Jni& j, Ctx&, jobject self, const auto& a) {
        return JValue(makeCtor(j, self, jobject(a[0])));
    });
    j.method(kCls, "getEnumConstants", "()[Ljava/lang/Object;", [classTarget](Jni& j, Ctx& x, jobject self, const auto&) {
        const std::string cls = classTarget(j, self);
        JValue res = 0;
        if (Dex::invokeFromJni(j, x, {cls, "values", "()[L" + cls + ";", true}, 0, {}, cls, false, res)) {
            return res;
        }
        return JValue(0);
    });
    j.method(kCls, "getResourceAsStream", "(Ljava/lang/String;)Ljava/io/InputStream;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        std::string rel = str(j, a[0]);
        if (!rel.empty() && rel[0] == '/') rel.erase(0, 1);
        const std::string path = x.proc.bundlePath() + "/" + rel;
        std::FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) {
            const std::string apath = x.proc.bundlePath() + "/assets/" + rel;
            f = std::fopen(apath.c_str(), "rb");
        }
        if (!f) return JValue(0);
        std::fseek(f, 0, SEEK_END);
        const long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> bytes(sz > 0 ? size_t(sz) : 0);
        if (sz > 0) (void)std::fread(bytes.data(), 1, size_t(sz), f);
        std::fclose(f);
        const jobject arr = j.newArray('B', uint32_t(bytes.size()));
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) o->data = std::move(bytes);
        }
        const jobject bis = j.newObject("java/io/ByteArrayInputStream");
        j.invoke(x, j.methodId("java/io/ByteArrayInputStream", "<init>", "([B)V", false), bis, {arr});
        return JValue(bis);
    });
    j.method(kCls, "getPackage", "()Ljava/lang/Package;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/lang/Package"));
    });
    j.method(kCls, "getDeclaredFields", "()[Ljava/lang/reflect/Field;", [](Jni& j, Ctx&, jobject, const auto&) {
        const jobject arr = j.newArray('L', 0);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(arr)) o->cls = "[Ljava/lang/reflect/Field;";
        return JValue(arr);
    });
    for (const char* mn : {"getDeclaredMethods", "getMethods"}) {
        j.method(kCls, mn, "()[Ljava/lang/reflect/Method;", [](Jni& j, Ctx&, jobject, const auto&) {
            const jobject arr = j.newArray('L', 0);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) o->cls = "[Ljava/lang/reflect/Method;";
            return JValue(arr);
        });
    }

    j.method("java/lang/reflect/AccessibleObject", "setAccessible", "(Z)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.accessible"] = a[0] & 1;
        return JValue(0);
    });
    j.method("java/lang/reflect/AccessibleObject", "isAccessible", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o && o->fields["hvm.accessible"] ? 1 : 0);
    });
    j.method("java/lang/reflect/Constructor", "getDeclaringClass", "()Ljava/lang/Class;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? o->fields["hvm.declaring"] : 0);
    });
    j.method("java/lang/reflect/Constructor", "newInstance", "([Ljava/lang/Object;)Ljava/lang/Object;", [classTarget](Jni& j, Ctx& x, jobject self, const auto& a) {
        jobject decl = 0;
        std::string sig = "()V";
        std::vector<JValue> cargs;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) {
                decl = jobject(o->fields["hvm.declaring"]);
                if (o->fields["hvm.sig"]) sig = str(j, o->fields["hvm.sig"]);
            }
            if (a[0]) {
                if (JObject* arr = j.get(jobject(a[0]))) {
                    for (jobject el : arr->refs) cargs.push_back(JValue(el));
                }
            }
        }
        const std::string cls = classTarget(j, decl);
        jobject out = 0;
        if (Dex::instantiateFromJni(j, x, cls, sig, cargs, out)) return JValue(out);
        out = j.newObject(cls);
        j.invoke(x, j.methodId(cls, "<init>", sig, false), out, cargs);
        return JValue(out);
    });

    // java.lang.reflect.Array & Modifier
    j.method("java/lang/reflect/Array", "newInstance", "(Ljava/lang/Class;I)Ljava/lang/Object;", [classTarget, toDescriptor](Jni& j, Ctx&, jobject, const auto& a) {
        const std::string comp = classTarget(j, jobject(a[0]));
        const int32_t len = std::max<int32_t>(0, int32_t(a[1]));
        const std::string desc = toDescriptor(comp);
        const char elem = (desc.size() == 1) ? desc[0] : 'L';
        const jobject arr = j.newArray(elem, uint32_t(len));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(arr)) o->cls = "[" + desc;
        return JValue(arr);
    });
    j.method("java/lang/reflect/Array", "getLength", "(Ljava/lang/Object;)I", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(jobject(a[0]));
        if (!o) return JValue(0);
        if (o->elem == 'L') return JValue(uint32_t(o->refs.size()));
        const size_t w = (o->elem == 'B' || o->elem == 'Z') ? 1 : (o->elem == 'S' || o->elem == 'C') ? 2 : (o->elem == 'J' || o->elem == 'D') ? 8 : 4;
        return JValue(uint32_t(o->data.size() / w));
    });
    j.method("java/lang/reflect/Array", "get", "(Ljava/lang/Object;I)Ljava/lang/Object;", [](Jni& j, Ctx&, jobject, const auto& a) {
        const uint32_t idx = uint32_t(int32_t(a[1]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(jobject(a[0]));
        if (o && o->elem == 'L' && idx < o->refs.size()) return JValue(o->refs[idx]);
        return JValue(0);
    });
    j.method("java/lang/reflect/Array", "set", "(Ljava/lang/Object;ILjava/lang/Object;)V", [](Jni& j, Ctx&, jobject, const auto& a) {
        const uint32_t idx = uint32_t(int32_t(a[1]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(jobject(a[0]));
        if (o && o->elem == 'L' && idx < o->refs.size()) o->refs[idx] = jobject(a[2]);
        return JValue(0);
    });
    j.method("java/lang/reflect/Modifier", "isPublic", "(I)Z", [](Jni&, Ctx&, jobject, const auto& a) { return JValue((a[0] & 0x0001) ? 1 : 0); });
    j.method("java/lang/reflect/Modifier", "isStatic", "(I)Z", [](Jni&, Ctx&, jobject, const auto& a) { return JValue((a[0] & 0x0008) ? 1 : 0); });
    j.method("java/lang/reflect/Modifier", "isTransient", "(I)Z", [](Jni&, Ctx&, jobject, const auto& a) { return JValue((a[0] & 0x0080) ? 1 : 0); });
}

} // namespace Hydrogen::Runtime::Android
