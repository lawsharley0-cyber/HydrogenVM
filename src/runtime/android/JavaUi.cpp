// HydrogenVM — Android UI, OS, and java.nio Buffer platform classes for JNI and DEX activities.
#include "runtime/android/Jni.hpp"
#include "runtime/android/Surface.hpp"
#include "runtime/android/dex/GuestScheduler.hpp"
#include "runtime/android/dex/JniBridge.hpp"
#include "runtime/Heap.hpp"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <unistd.h>
#include <unordered_map>

namespace fs = std::filesystem;

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

jobject newFileObj(Jni& j, const std::string& guestPath) {
    const jobject f = j.newObject("java/io/File");
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(f)) o->str = guestPath;
    return f;
}

std::string ensureDir(Process& p, const fs::path& host) {
    std::error_code ec;
    fs::create_directories(host, ec);
    return p.guestPath(host.string());
}

// Guest-backed or array-backed java.nio.Buffer state. Direct buffers store hvm.address and
// hvm.capacity on the JObject so JNI GetDirectBufferAddress/Capacity see the exact same memory.
struct NioBuf {
    uint32_t addr = 0;         // guest heap address for direct buffers (byte base of this view)
    jobject array = 0;         // backing byte[] for ByteBuffer.wrap
    uint32_t byteOffset = 0;   // offset into backing array (or 0 for direct views)
    uint32_t capacity = 0;     // capacity in elements
    uint32_t position = 0;     // current position in elements
    uint32_t limit = 0;        // current limit in elements
    uint32_t elemSize = 1;     // 1 (byte), 2 (short/char), 4 (int/float), 8 (long/double)
    bool direct = false;
    bool littleEndian = false; // Java ByteBuffer defaults to BIG_ENDIAN
};

std::shared_ptr<NioBuf> nioOf(Jni& j, jobject self) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    JObject* o = j.get(self);
    if (!o) return nullptr;
    if (auto* p = std::any_cast<std::shared_ptr<NioBuf>>(&o->host)) return *p;
    // Buffers created via JNI NewDirectByteBuffer only set hvm.address/hvm.capacity; adopt lazily.
    if (o->fields.count("hvm.address")) {
        auto b = std::make_shared<NioBuf>();
        b->addr = uint32_t(o->fields["hvm.address"]);
        b->capacity = uint32_t(o->fields["hvm.capacity"]);
        b->limit = b->capacity;
        b->elemSize = 1;
        b->direct = true;
        o->host = b;
        return b;
    }
    return nullptr;
}

void syncDirectFields(Jni& j, jobject h, const NioBuf& b) {
    if (!b.direct) return;
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(h)) {
        o->fields["hvm.address"] = b.addr;
        o->fields["hvm.capacity"] = b.capacity;
        o->fields["hvm.position"] = b.position;
        o->fields["hvm.limit"] = b.limit;
        o->fields["hvm.elemSize"] = b.elemSize;
    }
}

uint8_t readByteAt(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos) {
    if (b.direct) return x.mem.read<uint8_t>(b.addr + bytePos);
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* arr = j.get(b.array); arr && size_t(b.byteOffset) + bytePos < arr->data.size())
        return arr->data[size_t(b.byteOffset) + bytePos];
    return 0;
}

void writeByteAt(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos, uint8_t v) {
    if (b.direct) { x.mem.write<uint8_t>(b.addr + bytePos, v); return; }
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* arr = j.get(b.array); arr && size_t(b.byteOffset) + bytePos < arr->data.size())
        arr->data[size_t(b.byteOffset) + bytePos] = v;
}

void readBytesAt(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos, uint8_t* dst, uint32_t count) {
    if (!count) return;
    if (b.direct) {
        std::memcpy(dst, x.mem.host(b.addr + bytePos), count);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* arr = j.get(b.array); arr && size_t(b.byteOffset) + bytePos + count <= arr->data.size())
        std::memcpy(dst, arr->data.data() + size_t(b.byteOffset) + bytePos, count);
}

void writeBytesAt(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos, const uint8_t* src, uint32_t count) {
    if (!count) return;
    if (b.direct) {
        std::memcpy(x.mem.host(b.addr + bytePos), src, count);
        return;
    }
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* arr = j.get(b.array); arr && size_t(b.byteOffset) + bytePos + count <= arr->data.size())
        std::memcpy(arr->data.data() + size_t(b.byteOffset) + bytePos, src, count);
}

uint16_t readU16(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos) {
    uint8_t raw[2] = {};
    readBytesAt(j, x, b, bytePos, raw, 2);
    return b.littleEndian ? uint16_t(raw[0] | (uint16_t(raw[1]) << 8))
                          : uint16_t((uint16_t(raw[0]) << 8) | raw[1]);
}

void writeU16(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos, uint16_t v) {
    uint8_t raw[2];
    if (b.littleEndian) { raw[0] = uint8_t(v); raw[1] = uint8_t(v >> 8); }
    else { raw[0] = uint8_t(v >> 8); raw[1] = uint8_t(v); }
    writeBytesAt(j, x, b, bytePos, raw, 2);
}

uint32_t readU32(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos) {
    uint8_t raw[4] = {};
    readBytesAt(j, x, b, bytePos, raw, 4);
    if (b.littleEndian)
        return uint32_t(raw[0]) | (uint32_t(raw[1]) << 8) | (uint32_t(raw[2]) << 16) | (uint32_t(raw[3]) << 24);
    return (uint32_t(raw[0]) << 24) | (uint32_t(raw[1]) << 16) | (uint32_t(raw[2]) << 8) | uint32_t(raw[3]);
}

void writeU32(Jni& j, Ctx& x, const NioBuf& b, uint32_t bytePos, uint32_t v) {
    uint8_t raw[4];
    if (b.littleEndian) {
        raw[0] = uint8_t(v); raw[1] = uint8_t(v >> 8); raw[2] = uint8_t(v >> 16); raw[3] = uint8_t(v >> 24);
    } else {
        raw[0] = uint8_t(v >> 24); raw[1] = uint8_t(v >> 16); raw[2] = uint8_t(v >> 8); raw[3] = uint8_t(v);
    }
    writeBytesAt(j, x, b, bytePos, raw, 4);
}

jobject makeViewBuffer(Jni& j, const std::string& cls, const NioBuf& src, uint32_t elemSize) {
    auto v = std::make_shared<NioBuf>();
    const uint32_t remBytes = (src.limit > src.position ? src.limit - src.position : 0) * src.elemSize;
    v->direct = src.direct;
    v->addr = src.direct ? src.addr + src.position * src.elemSize : 0;
    v->array = src.array;
    v->byteOffset = src.byteOffset + src.position * src.elemSize;
    v->elemSize = elemSize;
    v->capacity = remBytes / elemSize;
    v->limit = v->capacity;
    v->position = 0;
    v->littleEndian = src.littleEndian;
    const jobject obj = j.newObject(cls);
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(obj)) o->host = v;
    }
    syncDirectFields(j, obj, *v);
    return obj;
}

void initLayoutParamsFields(Jni& j, jobject obj, int32_t w, int32_t h) {
    std::lock_guard<std::recursive_mutex> lk(j.mu);
    if (JObject* o = j.get(obj)) {
        o->fields["width"] = uint32_t(w);
        o->fields["height"] = uint32_t(h);
        o->fields["flags"] = 0;
        o->fields["screenBrightness"] = jfloat(-1.0f);
        o->fields["layoutInDisplayCutoutMode"] = 0;
        o->fields["format"] = 0;
        o->fields["gravity"] = 0;
        o->fields["type"] = 0;
        o->fields["x"] = 0;
        o->fields["y"] = 0;
    }
}

void registerNioBuffers(Jni& j) {
    j.superclasses["java/nio/DirectByteBuffer"] = "java/nio/ByteBuffer";
    j.superclasses["java/nio/MappedByteBuffer"] = "java/nio/ByteBuffer";
    j.superclasses["java/nio/ByteBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/FloatBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/IntBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/ShortBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/CharBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/DoubleBuffer"] = "java/nio/Buffer";
    j.superclasses["java/nio/LongBuffer"] = "java/nio/Buffer";

    // ByteOrder
    const jobject be = j.newObject("java/nio/ByteOrder");
    const jobject le = j.newObject("java/nio/ByteOrder");
    {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(be)) { o->permanent = true; o->str = "BIG_ENDIAN"; o->fields["hvm.le"] = 0; }
        if (JObject* o = j.get(le)) { o->permanent = true; o->str = "LITTLE_ENDIAN"; o->fields["hvm.le"] = 1; }
        j.staticFields["java/nio/ByteOrder.BIG_ENDIAN"] = be;
        j.staticFields["java/nio/ByteOrder.LITTLE_ENDIAN"] = le;
    }
    j.method("java/nio/ByteOrder", "nativeOrder", "()Ljava/nio/ByteOrder;",
             [le](Jni&, Ctx&, jobject, const auto&) { return JValue(le); });
    j.method("java/nio/ByteOrder", "toString", "()Ljava/lang/String;",
             [](Jni& j, Ctx&, jobject self, const auto&) { return JValue(j.newString(j.string(self))); });

    // java.nio.charset.Charset
    j.method("java/nio/charset/Charset", "forName", "(Ljava/lang/String;)Ljava/nio/charset/Charset;",
             [](Jni& j, Ctx&, jobject, const auto& a) {
                 const jobject cs = j.newObject("java/nio/charset/Charset");
                 std::lock_guard<std::recursive_mutex> lk(j.mu);
                 if (JObject* o = j.get(cs)) o->str = str(j, a[0]);
                 return JValue(cs);
             });
    j.method("java/nio/charset/Charset", "name", "()Ljava/lang/String;",
             [](Jni& j, Ctx&, jobject self, const auto&) { return JValue(j.newString(j.string(self))); });

    // java.nio.Buffer common methods (inherited by ByteBuffer/FloatBuffer/IntBuffer/ShortBuffer).
    const std::string kBuf = "java/nio/Buffer";
    j.method(kBuf, "capacity", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? b->capacity : 0);
    });
    j.method(kBuf, "position", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? b->position : 0);
    });
    j.method(kBuf, "position", "(I)Ljava/nio/Buffer;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) {
            b->position = std::min<uint32_t>(uint32_t(int32_t(a[0])), b->limit);
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBuf, "limit", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? b->limit : 0);
    });
    j.method(kBuf, "limit", "(I)Ljava/nio/Buffer;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) {
            b->limit = std::min<uint32_t>(uint32_t(int32_t(a[0])), b->capacity);
            if (b->position > b->limit) b->position = b->limit;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBuf, "remaining", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b && b->limit > b->position ? b->limit - b->position : 0);
    });
    j.method(kBuf, "hasRemaining", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b && b->position < b->limit ? 1 : 0);
    });
    j.method(kBuf, "clear", "()Ljava/nio/Buffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto b = nioOf(j, self)) {
            b->position = 0;
            b->limit = b->capacity;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBuf, "flip", "()Ljava/nio/Buffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto b = nioOf(j, self)) {
            b->limit = b->position;
            b->position = 0;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBuf, "rewind", "()Ljava/nio/Buffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto b = nioOf(j, self)) {
            b->position = 0;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBuf, "isDirect", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b && b->direct ? 1 : 0);
    });

    // java.nio.ByteBuffer
    const std::string kBB = "java/nio/ByteBuffer";
    j.method(kBB, "allocateDirect", "(I)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const uint32_t cap = uint32_t(std::max<int32_t>(0, int32_t(a[0])));
        const uint32_t addr = x.proc.heap().allocZeroed(std::max<uint32_t>(cap, 16));
        auto b = std::make_shared<NioBuf>();
        b->addr = addr;
        b->capacity = cap;
        b->limit = cap;
        b->elemSize = 1;
        b->direct = true;
        b->littleEndian = false;
        const jobject obj = j.newObject("java/nio/DirectByteBuffer");
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(obj)) o->host = b;
        }
        syncDirectFields(j, obj, *b);
        return JValue(obj);
    });
    j.method(kBB, "allocate", "(I)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx&, jobject, const auto& a) {
        const uint32_t cap = uint32_t(std::max<int32_t>(0, int32_t(a[0])));
        const jobject arr = j.newArray('B', cap);
        auto b = std::make_shared<NioBuf>();
        b->array = arr;
        b->capacity = cap;
        b->limit = cap;
        b->elemSize = 1;
        b->direct = false;
        const jobject obj = j.newObject("java/nio/ByteBuffer");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(obj)) o->host = b;
        return JValue(obj);
    });
    j.method(kBB, "wrap", "([B)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx&, jobject, const auto& a) {
        const jobject arr = jobject(a[0]);
        uint32_t cap = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) cap = uint32_t(o->data.size());
        }
        auto b = std::make_shared<NioBuf>();
        b->array = arr;
        b->capacity = cap;
        b->limit = cap;
        b->elemSize = 1;
        const jobject obj = j.newObject("java/nio/ByteBuffer");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(obj)) o->host = b;
        return JValue(obj);
    });
    j.method(kBB, "wrap", "([BII)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx&, jobject, const auto& a) {
        const jobject arr = jobject(a[0]);
        const uint32_t off = uint32_t(std::max<int32_t>(0, int32_t(a[1])));
        const uint32_t len = uint32_t(std::max<int32_t>(0, int32_t(a[2])));
        uint32_t cap = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) cap = uint32_t(o->data.size());
        }
        auto b = std::make_shared<NioBuf>();
        b->array = arr;
        b->capacity = cap;
        b->position = std::min(off, cap);
        b->limit = std::min(off + len, cap);
        b->elemSize = 1;
        const jobject obj = j.newObject("java/nio/ByteBuffer");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(obj)) o->host = b;
        return JValue(obj);
    });
    j.method(kBB, "order", "()Ljava/nio/ByteOrder;", [be, le](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b && b->littleEndian ? le : be);
    });
    j.method(kBB, "order", "(Ljava/nio/ByteOrder;)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* ord = j.get(jobject(a[0]))) b->littleEndian = ord->fields["hvm.le"] != 0;
        }
        return JValue(self);
    });
    j.method(kBB, "slice", "()Ljava/nio/ByteBuffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b) return JValue(0);
        return JValue(makeViewBuffer(j, b->direct ? "java/nio/DirectByteBuffer" : "java/nio/ByteBuffer", *b, 1));
    });
    j.method(kBB, "asFloatBuffer", "()Ljava/nio/FloatBuffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? makeViewBuffer(j, "java/nio/FloatBuffer", *b, 4) : 0);
    });
    j.method(kBB, "asIntBuffer", "()Ljava/nio/IntBuffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? makeViewBuffer(j, "java/nio/IntBuffer", *b, 4) : 0);
    });
    j.method(kBB, "asShortBuffer", "()Ljava/nio/ShortBuffer;", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? makeViewBuffer(j, "java/nio/ShortBuffer", *b, 2) : 0);
    });
    j.method(kBB, "get", "()B", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b || b->position >= b->limit) return JValue(0);
        const int8_t v = int8_t(readByteAt(j, x, *b, b->position++));
        syncDirectFields(j, self, *b);
        return JValue(uint32_t(int32_t(v)));
    });
    j.method(kBB, "get", "(I)B", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        if (!b) return JValue(0);
        const int8_t v = int8_t(readByteAt(j, x, *b, uint32_t(int32_t(a[0]))));
        return JValue(uint32_t(int32_t(v)));
    });
    j.method(kBB, "get", "([B)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        if (!b) return JValue(self);
        std::vector<uint8_t> tmp;
        uint32_t len = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* dst = j.get(jobject(a[0]))) len = uint32_t(dst->data.size());
        }
        len = std::min(len, b->limit > b->position ? b->limit - b->position : 0);
        tmp.resize(len);
        readBytesAt(j, x, *b, b->position, tmp.data(), len);
        b->position += len;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* dst = j.get(jobject(a[0])); dst && len <= dst->data.size())
                std::memcpy(dst->data.data(), tmp.data(), len);
        }
        syncDirectFields(j, self, *b);
        return JValue(self);
    });
    j.method(kBB, "get", "([BII)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        if (!b) return JValue(self);
        const uint32_t off = uint32_t(int32_t(a[1]));
        uint32_t len = uint32_t(int32_t(a[2]));
        len = std::min(len, b->limit > b->position ? b->limit - b->position : 0);
        std::vector<uint8_t> tmp(len);
        readBytesAt(j, x, *b, b->position, tmp.data(), len);
        b->position += len;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* dst = j.get(jobject(a[0])); dst && size_t(off) + len <= dst->data.size())
                std::memcpy(dst->data.data() + off, tmp.data(), len);
        }
        syncDirectFields(j, self, *b);
        return JValue(self);
    });
    j.method(kBB, "getInt", "()I", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b || b->position + 4 > b->limit) return JValue(0);
        const uint32_t v = readU32(j, x, *b, b->position);
        b->position += 4;
        syncDirectFields(j, self, *b);
        return JValue(v);
    });
    j.method(kBB, "getInt", "(I)I", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        return JValue(b ? readU32(j, x, *b, uint32_t(int32_t(a[0]))) : 0);
    });
    j.method(kBB, "put", "(B)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self); b && b->position < b->limit) {
            writeByteAt(j, x, *b, b->position++, uint8_t(a[0]));
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBB, "put", "(IB)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) writeByteAt(j, x, *b, uint32_t(int32_t(a[0])), uint8_t(a[1]));
        return JValue(self);
    });
    j.method(kBB, "put", "([B)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) {
            std::vector<uint8_t> src;
            {
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* arr = j.get(jobject(a[0]))) src = arr->data;
            }
            const uint32_t n = std::min<uint32_t>(uint32_t(src.size()), b->limit > b->position ? b->limit - b->position : 0);
            writeBytesAt(j, x, *b, b->position, src.data(), n);
            b->position += n;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBB, "put", "([BII)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) {
            const uint32_t off = uint32_t(int32_t(a[1]));
            uint32_t len = uint32_t(int32_t(a[2]));
            std::vector<uint8_t> src;
            {
                std::lock_guard<std::recursive_mutex> lk(j.mu);
                if (JObject* arr = j.get(jobject(a[0])); arr && size_t(off) + len <= arr->data.size())
                    src.assign(arr->data.begin() + off, arr->data.begin() + off + len);
            }
            const uint32_t n = std::min<uint32_t>(uint32_t(src.size()), b->limit > b->position ? b->limit - b->position : 0);
            writeBytesAt(j, x, *b, b->position, src.data(), n);
            b->position += n;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBB, "put", "(Ljava/nio/ByteBuffer;)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto dst = nioOf(j, self);
        auto src = nioOf(j, jobject(a[0]));
        if (dst && src) {
            const uint32_t srcRem = src->limit > src->position ? src->limit - src->position : 0;
            const uint32_t dstRem = dst->limit > dst->position ? dst->limit - dst->position : 0;
            const uint32_t n = std::min(srcRem, dstRem);
            std::vector<uint8_t> tmp(n);
            readBytesAt(j, x, *src, src->position, tmp.data(), n);
            src->position += n;
            writeBytesAt(j, x, *dst, dst->position, tmp.data(), n);
            dst->position += n;
            syncDirectFields(j, jobject(a[0]), *src);
            syncDirectFields(j, self, *dst);
        }
        return JValue(self);
    });
    j.method(kBB, "putInt", "(I)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self); b && b->position + 4 <= b->limit) {
            writeU32(j, x, *b, b->position, uint32_t(a[0]));
            b->position += 4;
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kBB, "putInt", "(II)Ljava/nio/ByteBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) writeU32(j, x, *b, uint32_t(int32_t(a[0])), uint32_t(a[1]));
        return JValue(self);
    });
    j.method(kBB, "hasArray", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b && !b->direct && b->array != 0 ? 1 : 0);
    });
    j.method(kBB, "array", "()[B", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? b->array : 0);
    });
    j.method(kBB, "arrayOffset", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        auto b = nioOf(j, self);
        return JValue(b ? b->byteOffset : 0);
    });

    // java.nio.FloatBuffer
    const std::string kFB = "java/nio/FloatBuffer";
    j.method(kFB, "get", "()F", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b || b->position >= b->limit) return JValue(0);
        const uint32_t v = readU32(j, x, *b, (b->position++) * 4);
        syncDirectFields(j, self, *b);
        return JValue(v);
    });
    j.method(kFB, "get", "(I)F", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        return JValue(b ? readU32(j, x, *b, uint32_t(int32_t(a[0])) * 4) : 0);
    });
    j.method(kFB, "put", "(F)Ljava/nio/FloatBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self); b && b->position < b->limit) {
            writeU32(j, x, *b, (b->position++) * 4, uint32_t(a[0]));
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kFB, "put", "(IF)Ljava/nio/FloatBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) writeU32(j, x, *b, uint32_t(int32_t(a[0])) * 4, uint32_t(a[1]));
        return JValue(self);
    });
    auto putFloats = [](Jni& j, Ctx& x, jobject self, jobject arrHandle, uint32_t off, uint32_t len, bool fullArray) {
        auto b = nioOf(j, self);
        if (!b) return;
        std::vector<uint32_t> vals;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            JObject* arr = j.get(arrHandle);
            if (!arr) return;
            const uint32_t total = uint32_t(arr->data.size() / 4);
            if (fullArray) { off = 0; len = total; }
            if (off + len > total) len = off < total ? total - off : 0;
            vals.resize(len);
            if (len) std::memcpy(vals.data(), arr->data.data() + size_t(off) * 4, size_t(len) * 4);
        }
        const uint32_t n = std::min<uint32_t>(uint32_t(vals.size()), b->limit > b->position ? b->limit - b->position : 0);
        for (uint32_t i = 0; i < n; ++i) writeU32(j, x, *b, (b->position++) * 4, vals[i]);
        syncDirectFields(j, self, *b);
    };
    j.method(kFB, "put", "([F)Ljava/nio/FloatBuffer;", [putFloats](Jni& j, Ctx& x, jobject self, const auto& a) {
        putFloats(j, x, self, jobject(a[0]), 0, 0, true);
        return JValue(self);
    });
    j.method(kFB, "put", "([FII)Ljava/nio/FloatBuffer;", [putFloats](Jni& j, Ctx& x, jobject self, const auto& a) {
        putFloats(j, x, self, jobject(a[0]), uint32_t(int32_t(a[1])), uint32_t(int32_t(a[2])), false);
        return JValue(self);
    });

    // java.nio.IntBuffer
    const std::string kIB = "java/nio/IntBuffer";
    j.method(kIB, "get", "()I", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b || b->position >= b->limit) return JValue(0);
        const uint32_t v = readU32(j, x, *b, (b->position++) * 4);
        syncDirectFields(j, self, *b);
        return JValue(v);
    });
    j.method(kIB, "get", "(I)I", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        return JValue(b ? readU32(j, x, *b, uint32_t(int32_t(a[0])) * 4) : 0);
    });
    j.method(kIB, "put", "(I)Ljava/nio/IntBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self); b && b->position < b->limit) {
            writeU32(j, x, *b, (b->position++) * 4, uint32_t(a[0]));
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kIB, "put", "(II)Ljava/nio/IntBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) writeU32(j, x, *b, uint32_t(int32_t(a[0])) * 4, uint32_t(a[1]));
        return JValue(self);
    });
    j.method(kIB, "put", "([I)Ljava/nio/IntBuffer;", [putFloats](Jni& j, Ctx& x, jobject self, const auto& a) {
        putFloats(j, x, self, jobject(a[0]), 0, 0, true);
        return JValue(self);
    });
    j.method(kIB, "put", "([III)Ljava/nio/IntBuffer;", [putFloats](Jni& j, Ctx& x, jobject self, const auto& a) {
        putFloats(j, x, self, jobject(a[0]), uint32_t(int32_t(a[1])), uint32_t(int32_t(a[2])), false);
        return JValue(self);
    });

    // java.nio.ShortBuffer
    const std::string kSB = "java/nio/ShortBuffer";
    j.method(kSB, "get", "()S", [](Jni& j, Ctx& x, jobject self, const auto&) {
        auto b = nioOf(j, self);
        if (!b || b->position >= b->limit) return JValue(0);
        const int16_t v = int16_t(readU16(j, x, *b, (b->position++) * 2));
        syncDirectFields(j, self, *b);
        return JValue(uint32_t(int32_t(v)));
    });
    j.method(kSB, "get", "(I)S", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto b = nioOf(j, self);
        const int16_t v = b ? int16_t(readU16(j, x, *b, uint32_t(int32_t(a[0])) * 2)) : 0;
        return JValue(uint32_t(int32_t(v)));
    });
    j.method(kSB, "put", "(S)Ljava/nio/ShortBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self); b && b->position < b->limit) {
            writeU16(j, x, *b, (b->position++) * 2, uint16_t(a[0]));
            syncDirectFields(j, self, *b);
        }
        return JValue(self);
    });
    j.method(kSB, "put", "(IS)Ljava/nio/ShortBuffer;", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        if (auto b = nioOf(j, self)) writeU16(j, x, *b, uint32_t(int32_t(a[0])) * 2, uint16_t(a[1]));
        return JValue(self);
    });
    auto putShorts = [](Jni& j, Ctx& x, jobject self, jobject arrHandle, uint32_t off, uint32_t len, bool fullArray) {
        auto b = nioOf(j, self);
        if (!b) return;
        std::vector<uint16_t> vals;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            JObject* arr = j.get(arrHandle);
            if (!arr) return;
            const uint32_t total = uint32_t(arr->data.size() / 2);
            if (fullArray) { off = 0; len = total; }
            if (off + len > total) len = off < total ? total - off : 0;
            vals.resize(len);
            if (len) std::memcpy(vals.data(), arr->data.data() + size_t(off) * 2, size_t(len) * 2);
        }
        const uint32_t n = std::min<uint32_t>(uint32_t(vals.size()), b->limit > b->position ? b->limit - b->position : 0);
        for (uint32_t i = 0; i < n; ++i) writeU16(j, x, *b, (b->position++) * 2, vals[i]);
        syncDirectFields(j, self, *b);
    };
    j.method(kSB, "put", "([S)Ljava/nio/ShortBuffer;", [putShorts](Jni& j, Ctx& x, jobject self, const auto& a) {
        putShorts(j, x, self, jobject(a[0]), 0, 0, true);
        return JValue(self);
    });
    j.method(kSB, "put", "([SII)Ljava/nio/ShortBuffer;", [putShorts](Jni& j, Ctx& x, jobject self, const auto& a) {
        putShorts(j, x, self, jobject(a[0]), uint32_t(int32_t(a[1])), uint32_t(int32_t(a[2])), false);
        return JValue(self);
    });
}

} // namespace

void registerJavaUi(Jni& j) {
    Process& p = j.proc;
    const Surface surf = surfaceFor(p);

    // Superclass hierarchy for Android views, layouts, widgets, and OS classes.
    j.superclasses["android/opengl/GLSurfaceView"] = "android/view/SurfaceView";
    j.superclasses["android/view/SurfaceView"] = "android/view/View";
    j.superclasses["android/widget/LinearLayout"] = "android/view/ViewGroup";
    j.superclasses["android/widget/RelativeLayout"] = "android/view/ViewGroup";
    j.superclasses["android/widget/TextView"] = "android/view/View";
    j.superclasses["android/widget/FrameLayout$LayoutParams"] = "android/view/ViewGroup$MarginLayoutParams";
    j.superclasses["android/widget/LinearLayout$LayoutParams"] = "android/view/ViewGroup$MarginLayoutParams";
    j.superclasses["android/view/ViewGroup$MarginLayoutParams"] = "android/view/ViewGroup$LayoutParams";
    j.superclasses["android/view/WindowManager$LayoutParams"] = "android/view/ViewGroup$LayoutParams";
    j.superclasses["android/os/Bundle"] = "android/os/BaseBundle";
    j.superclasses["android/graphics/drawable/ColorDrawable"] = "android/graphics/drawable/Drawable";
    j.superclasses["android/os/HandlerThread"] = "java/lang/Thread";

    // android.view.Window and WindowManager$LayoutParams
    const std::string kWin = "android/view/Window";
    const std::string kWlp = "android/view/WindowManager$LayoutParams";
    j.method(kWlp, "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        initLayoutParamsFields(j, self, -1, -1);
        return JValue(0);
    });
    j.method(kWlp, "<init>", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        initLayoutParamsFields(j, self, int32_t(a[0]), int32_t(a[1]));
        return JValue(0);
    });
    j.method(kWin, "setFlags", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            const uint32_t flags = uint32_t(a[0]), mask = uint32_t(a[1]);
            o->fields["hvm.flags"] = (o->fields["hvm.flags"] & ~uint64_t(mask)) | (flags & mask);
        }
        return JValue(0);
    });
    j.method(kWin, "addFlags", "(I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.flags"] |= uint32_t(a[0]);
        return JValue(0);
    });
    j.method(kWin, "clearFlags", "(I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.flags"] &= ~uint64_t(uint32_t(a[0]));
        return JValue(0);
    });
    j.method(kWin, "getAttributes", "()Landroid/view/WindowManager$LayoutParams;", [kWlp](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* w = j.get(self);
        if (w && w->fields["hvm.attrs"]) return JValue(w->fields["hvm.attrs"]);
        const jobject attrs = j.newObject(kWlp);
        initLayoutParamsFields(j, attrs, -1, -1);
        if (JObject* a = j.get(attrs)) a->permanent = true;
        if (w) w->fields["hvm.attrs"] = attrs;
        return JValue(attrs);
    });
    j.method(kWin, "setAttributes", "(Landroid/view/WindowManager$LayoutParams;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* w = j.get(self)) w->fields["hvm.attrs"] = a[0];
        return JValue(0);
    });
    j.method(kWin, "getDecorView", "()Landroid/view/View;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "android/view/View"));
    });
    j.method(kWin, "setSoftInputMode", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.app.Activity lifecycle and view hosting
    const std::string kAct = "android/app/Activity";
    j.method(kAct, "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    for (const char* lc : {"onStart", "onResume", "onPause", "onStop", "onRestart", "onDestroy"})
        j.method(kAct, lc, "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "onWindowFocusChanged", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "onConfigurationChanged", "(Landroid/content/res/Configuration;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "onActivityResult", "(IILandroid/content/Intent;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "requestWindowFeature", "(I)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kAct, "setRequestedOrientation", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "setVolumeControlStream", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "isFinishing", "()Z", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o && o->fields["hvm.finishing"] ? 1 : 0);
    });
    j.method(kAct, "finish", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.finishing"] = 1;
        j.staticFields["hvm.activityFinished"] = 1;
        return JValue(0);
    });
    j.method(kAct, "finishAndRemoveTask", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.finishing"] = 1;
        j.staticFields["hvm.activityFinished"] = 1;
        return JValue(0);
    });
    j.method(kAct, "isInMultiWindowMode", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAct, "setContentView", "(Landroid/view/View;)V", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        j.staticFields["hvm.contentView"] = a[0];
        return JValue(0);
    });
    j.method(kAct, "setContentView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", [](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        j.staticFields["hvm.contentView"] = a[0];
        return JValue(0);
    });
    j.method(kAct, "findViewById", "(I)Landroid/view/View;", [](Jni& j, Ctx&, jobject, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        return JValue(j.staticFields["hvm.contentView"] ? j.staticFields["hvm.contentView"] : singleton(j, "android/view/View"));
    });
    for (const char* ctx : {"android/content/Context", "android/app/Activity"}) {
        j.method(ctx, "startActivity", "(Landroid/content/Intent;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
        j.method(ctx, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;", [&p](Jni& j, Ctx& x, jobject, const auto&) {
            const jobject pm = singleton(j, "android/content/pm/PackageManager");
            const uint32_t mid = j.methodId("android/content/pm/PackageManager", "getApplicationInfo",
                                            "(Ljava/lang/String;I)Landroid/content/pm/ApplicationInfo;", false);
            const auto* info = p.infoPlist();
            const std::string pkg = info ? info->getString("CFBundleIdentifier", "app") : std::string("app");
            return j.invoke(x, mid, pm, {j.newString(pkg), 0});
        });
        j.method(ctx, "getCodeCacheDir", "()Ljava/io/File;", [&p](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(newFileObj(j, ensureDir(p, fs::path(p.homePath()) / "Library/Caches/code_cache")));
        });
        j.method(ctx, "getString", "(I)Ljava/lang/String;", [](Jni& j, Ctx&, jobject, const auto&) {
            return JValue(j.newString(""));
        });
        j.method(ctx, "createDeviceProtectedStorageContext", "()Landroid/content/Context;", [](Jni&, Ctx&, jobject self, const auto&) {
            return JValue(self);
        });
    }

    // android.view.View, ViewGroup, LayoutParams, Widgets
    const std::string kView = "android/view/View";
    j.method(kView, "<init>", "(Landroid/content/Context;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "getRootView", "()Landroid/view/View;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kView, "getViewTreeObserver", "()Landroid/view/ViewTreeObserver;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "android/view/ViewTreeObserver"));
    });
    j.method(kView, "getWindowToken", "()Landroid/os/IBinder;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "getWindowVisibleDisplayFrame", "(Landroid/graphics/Rect;)V", [surf](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* r = j.get(jobject(a[0]))) {
            r->fields["left"] = 0;
            r->fields["top"] = 0;
            r->fields["right"] = uint32_t(surf.width);
            r->fields["bottom"] = uint32_t(surf.height);
        }
        return JValue(0);
    });
    j.method(kView, "isAttachedToWindow", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kView, "requestFocus", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kView, "setFocusable", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setFocusableInTouchMode", "(Z)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setLayoutParams", "(Landroid/view/ViewGroup$LayoutParams;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setMeasuredDimension", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            o->fields["hvm.measuredW"] = uint32_t(a[0]);
            o->fields["hvm.measuredH"] = uint32_t(a[1]);
        }
        return JValue(0);
    });
    j.method(kView, "getWidth", "()I", [surf](Jni&, Ctx&, jobject, const auto&) { return JValue(uint32_t(surf.width)); });
    j.method(kView, "getHeight", "()I", [surf](Jni&, Ctx&, jobject, const auto&) { return JValue(uint32_t(surf.height)); });
    j.method(kView, "setOnTouchListener", "(Landroid/view/View$OnTouchListener;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.touchListener"] = a[0];
        if (a[0]) {
            j.staticFields["hvm.touchListener"] = a[0];
            j.staticFields["hvm.touchView"] = self;
        }
        return JValue(0);
    });
    j.method(kView, "setOnKeyListener", "(Landroid/view/View$OnKeyListener;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.keyListener"] = a[0];
        return JValue(0);
    });
    j.method(kView, "setOnGenericMotionListener", "(Landroid/view/View$OnGenericMotionListener;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setOnSystemUiVisibilityChangeListener", "(Landroid/view/View$OnSystemUiVisibilityChangeListener;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setOnApplyWindowInsetsListener", "(Landroid/view/View$OnApplyWindowInsetsListener;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "setSystemUiVisibility", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "requestApplyInsets", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kView, "post", "(Ljava/lang/Runnable;)Z", [](Jni& j, Ctx& x, jobject, const auto& a) {
        const jobject r = jobject(a[0]);
        if (r) j.invoke(x, j.methodId("java/lang/Runnable", "run", "()V", false), r, {});
        return JValue(1);
    });
    j.method(kView, "setTag", "(ILjava/lang/Object;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.tag." + std::to_string(int32_t(a[0]))] = a[1];
        return JValue(0);
    });
    j.method(kView, "getTag", "(I)Ljava/lang/Object;", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            auto it = o->fields.find("hvm.tag." + std::to_string(int32_t(a[0])));
            if (it != o->fields.end()) return JValue(it->second);
        }
        return JValue(0);
    });
    j.method("android/view/View$MeasureSpec", "getSize", "(I)I", [](Jni&, Ctx&, jobject, const auto& a) {
        return JValue(uint32_t(a[0]) & 0x3FFFFFFFu);
    });
    j.method("android/view/View$MeasureSpec", "getMode", "(I)I", [](Jni&, Ctx&, jobject, const auto& a) {
        return JValue(uint32_t(a[0]) & 0xC0000000u);
    });
    j.method("android/view/ViewConfiguration", "getLongPressTimeout", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(500); });
    j.method("android/view/ViewTreeObserver", "addOnGlobalLayoutListener", "(Landroid/view/ViewTreeObserver$OnGlobalLayoutListener;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // LayoutParams & containers
    for (const char* lp : {"android/view/ViewGroup$LayoutParams", "android/view/ViewGroup$MarginLayoutParams",
                           "android/widget/FrameLayout$LayoutParams", "android/widget/LinearLayout$LayoutParams"}) {
        j.method(lp, "<init>", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
            initLayoutParamsFields(j, self, int32_t(a[0]), int32_t(a[1]));
            return JValue(0);
        });
    }
    j.method("android/widget/FrameLayout", "<init>", "(Landroid/content/Context;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/LinearLayout", "<init>", "(Landroid/content/Context;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/ViewGroup", "addView", "(Landroid/view/View;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/ViewGroup", "addView", "(Landroid/view/View;Landroid/view/ViewGroup$LayoutParams;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "<init>", "(Landroid/content/Context;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setText", "(Ljava/lang/CharSequence;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setTextColor", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setTextSize", "(F)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setGravity", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setPadding", "(IIII)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/TextView", "setTypeface", "(Landroid/graphics/Typeface;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "<init>", "(Landroid/content/Context;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "isShowing", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "dismiss", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setBackgroundDrawable", "(Landroid/graphics/drawable/Drawable;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setContentView", "(Landroid/view/View;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setWidth", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setHeight", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setInputMethodMode", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "setSoftInputMode", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/widget/PopupWindow", "showAtLocation", "(Landroid/view/View;III)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.graphics.Point, Rect, ColorDrawable, Typeface, and Display.getSize
    j.method("android/graphics/Point", "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) { o->fields["x"] = 0; o->fields["y"] = 0; }
        return JValue(0);
    });
    j.method("android/graphics/Point", "<init>", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) { o->fields["x"] = uint32_t(a[0]); o->fields["y"] = uint32_t(a[1]); }
        return JValue(0);
    });
    j.method("android/view/Display", "getSize", "(Landroid/graphics/Point;)V", [surf](Jni& j, Ctx&, jobject, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* pt = j.get(jobject(a[0]))) {
            pt->fields["x"] = uint32_t(surf.width);
            pt->fields["y"] = uint32_t(surf.height);
        }
        return JValue(0);
    });
    j.method("android/graphics/Rect", "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            o->fields["left"] = 0; o->fields["top"] = 0; o->fields["right"] = 0; o->fields["bottom"] = 0;
        }
        return JValue(0);
    });
    j.method("android/graphics/Rect", "width", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) return JValue(uint32_t(int32_t(o->fields["right"]) - int32_t(o->fields["left"])));
        return JValue(0);
    });
    j.method("android/graphics/Rect", "height", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) return JValue(uint32_t(int32_t(o->fields["bottom"]) - int32_t(o->fields["top"])));
        return JValue(0);
    });
    j.method("android/graphics/drawable/ColorDrawable", "<init>", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/graphics/Typeface", "createFromAsset",
             "(Landroid/content/res/AssetManager;Ljava/lang/String;)Landroid/graphics/Typeface;",
             [](Jni& j, Ctx&, jobject, const auto&) { return JValue(singleton(j, "android/graphics/Typeface")); });

    // android.content.res.Resources.getConfiguration()
    j.method("android/content/res/Resources", "getConfiguration", "()Landroid/content/res/Configuration;", [surf](Jni& j, Ctx&, jobject, const auto&) {
        const jobject cfg = j.newObject("android/content/res/Configuration");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(cfg)) {
            o->fields["orientation"] = surf.portrait ? 1u : 2u;   // PORTRAIT=1, LANDSCAPE=2
            o->fields["keyboard"] = 1u;                           // KEYBOARD_NOKEYS=1
            o->fields["keyboardHidden"] = 2u;                     // KEYBOARDHIDDEN_YES=2
            o->fields["hardKeyboardHidden"] = 2u;                 // HARDKEYBOARDHIDDEN_YES=2
            o->fields["navigation"] = 1u;                         // NAVIGATION_NONAV=1
            o->fields["navigationHidden"] = 2u;
            o->fields["touchscreen"] = 3u;                        // TOUCHSCREEN_FINGER=3
            o->fields["screenLayout"] = 2u;                       // SCREENLAYOUT_SIZE_NORMAL=2
            o->fields["uiMode"] = 1u;                             // UI_MODE_TYPE_NORMAL=1
            o->fields["densityDpi"] = 320u;
            o->fields["fontScale"] = jfloat(1.0f);
            o->fields["locale"] = singleton(j, "java/util/Locale");
        }
        return JValue(cfg);
    });

    // android.os.Handler, Looper, HandlerThread, SystemClock, Trace, Process, Vibrator, BaseBundle
    j.method("android/os/Looper", "getMainLooper", "()Landroid/os/Looper;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "android/os/Looper"));
    });
    j.method("android/os/Handler", "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Handler", "<init>", "(Landroid/os/Looper;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Handler", "createAsync", "(Landroid/os/Looper;)Landroid/os/Handler;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("android/os/Handler"));
    });
    j.method("android/os/Handler", "post", "(Ljava/lang/Runnable;)Z", [](Jni& j, Ctx& x, jobject, const auto& a) {
        if (const jobject r = jobject(a[0])) j.invoke(x, j.methodId("java/lang/Runnable", "run", "()V", false), r, {});
        return JValue(1);
    });
    j.method("android/os/Handler", "postDelayed", "(Ljava/lang/Runnable;J)Z", [](Jni& j, Ctx& x, jobject, const auto& a) {
        if (const jobject r = jobject(a[0])) j.invoke(x, j.methodId("java/lang/Runnable", "run", "()V", false), r, {});
        return JValue(1);
    });
    j.method("android/os/HandlerThread", "<init>", "(Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/HandlerThread", "getLooper", "()Landroid/os/Looper;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "android/os/Looper"));
    });
    j.method("android/os/HandlerThread", "quit", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method("android/os/SystemClock", "uptimeMillis", "()J", [](Jni&, Ctx&, jobject, const auto&) {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return JValue(uint64_t(ts.tv_sec) * 1000u + uint64_t(ts.tv_nsec) / 1000000u);
    });
    j.method("android/os/Trace", "isEnabled", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Trace", "beginSection", "(Ljava/lang/String;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Trace", "endSection", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Process", "myPid", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1000); });
    j.method("android/os/Process", "killProcess", "(I)V", [](Jni& j, Ctx&, jobject, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        j.staticFields["hvm.activityFinished"] = 1;
        return JValue(0);
    });
    j.method("android/os/Process", "sendSignal", "(II)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Vibrator", "hasVibrator", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Vibrator", "hasAmplitudeControl", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/Vibrator", "vibrate", "(J)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/BaseBundle", "getString", "(Ljava/lang/String;)Ljava/lang/String;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/os/BaseBundle", "getString", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;", [](Jni&, Ctx&, jobject, const auto& a) { return a[1]; });
    j.method("android/content/Intent", "getExtras", "()Landroid/os/Bundle;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.util.Log & SparseIntArray
    for (const char* lvl : {"v", "d", "i", "w", "e"}) {
        const std::string tagLvl = lvl;
        const auto logFn = [tagLvl](Jni& j, Ctx& x, jobject, const auto& a) {
            const LogLevel ll = (tagLvl == "e") ? LogLevel::Error : (tagLvl == "w") ? LogLevel::Warning : LogLevel::Info;
            std::string msg = str(j, a[1]);
            if (a.size() >= 3 && a[2]) {
                std::string trCls;
                { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(jobject(a[2]))) trCls = o->cls; }
                const jobject m = jobject(j.invoke(x, j.methodId("java/lang/Throwable", "getMessage", "()Ljava/lang/String;", false), jobject(a[2]), {}));
                const std::string trMsg = m ? str(j, m) : "";
                if (!msg.empty()) msg += " — ";
                msg += trCls + (trMsg.empty() ? "" : ": " + trMsg);
            }
            x.proc.logf(ll, "AndroidLog/%s [%s]: %s", tagLvl.c_str(), str(j, a[0]).c_str(), msg.c_str());
            return JValue(0);
        };
        j.method("android/util/Log", lvl, "(Ljava/lang/String;Ljava/lang/String;)I", logFn);
        j.method("android/util/Log", lvl, "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/Throwable;)I", logFn);
    }
    struct SparseMap { std::unordered_map<int32_t, int32_t> entries; };
    j.method("android/util/SparseIntArray", "<init>", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = std::make_shared<SparseMap>();
        return JValue(0);
    });
    j.method("android/util/SparseIntArray", "put", "(II)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* m = std::any_cast<std::shared_ptr<SparseMap>>(&o->host)) (*m)->entries[int32_t(a[0])] = int32_t(a[1]);
        return JValue(0);
    });
    j.method("android/util/SparseIntArray", "get", "(I)I", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* m = std::any_cast<std::shared_ptr<SparseMap>>(&o->host))
                if (auto it = (*m)->entries.find(int32_t(a[0])); it != (*m)->entries.end()) return JValue(uint32_t(it->second));
        return JValue(0);
    });
    j.method("android/util/SparseIntArray", "get", "(II)I", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* m = std::any_cast<std::shared_ptr<SparseMap>>(&o->host))
                if (auto it = (*m)->entries.find(int32_t(a[0])); it != (*m)->entries.end()) return JValue(uint32_t(it->second));
        return a[1];
    });

    // android.view.InputDevice & MotionEvent extra getters
    j.method("android/view/InputDevice", "getDeviceIds", "()[I", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newArray('I', 0));
    });
    j.method("android/view/InputDevice", "getDevice", "(I)Landroid/view/InputDevice;", [](Jni&, Ctx&, jobject, const auto&) {
        return JValue(0);
    });
    j.method("android/view/MotionEvent", "getDeviceId", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/MotionEvent", "getSource", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0x1002); });
    j.method("android/view/MotionEvent", "getButtonState", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/MotionEvent", "getAxisValue", "(I)F", [](Jni&, Ctx&, jobject, const auto&) { return jfloat(0.0f); });

    // android.content.ClipboardManager
    j.method("android/content/ClipboardManager", "hasPrimaryClip", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/content/ClipboardManager", "getPrimaryClip", "()Landroid/content/ClipData;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/content/ClipboardManager", "setPrimaryClip", "(Landroid/content/ClipData;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.view.inputmethod.InputMethodManager
    j.method("android/view/inputmethod/InputMethodManager", "hideSoftInputFromWindow", "(Landroid/os/IBinder;I)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method("android/view/inputmethod/InputMethodManager", "showSoftInput", "(Landroid/view/View;I)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("android/view/inputmethod/InputMethodManager", "restartInput", "(Landroid/view/View;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.content.res.AssetFileDescriptor.getDeclaredLength
    j.method("android/content/res/AssetFileDescriptor", "getDeclaredLength", "()J", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? o->fields["hvm.length"] : 0);
    });

    // android.media.AudioAttributes$Builder & MediaPlayer.setAudioAttributes
    const std::string kAab = "android/media/AudioAttributes$Builder";
    j.method(kAab, "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kAab, "setUsage", "(I)Landroid/media/AudioAttributes$Builder;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kAab, "setContentType", "(I)Landroid/media/AudioAttributes$Builder;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kAab, "setLegacyStreamType", "(I)Landroid/media/AudioAttributes$Builder;", [](Jni&, Ctx&, jobject self, const auto&) { return JValue(self); });
    j.method(kAab, "build", "()Landroid/media/AudioAttributes;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("android/media/AudioAttributes"));
    });
    j.method("android/media/MediaPlayer", "setAudioAttributes", "(Landroid/media/AudioAttributes;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // android.hardware.SensorManager, Sensor, InputManager
    j.method("android/hardware/Sensor", "getType", "()I", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    const std::string kSm = "android/hardware/SensorManager";
    j.method(kSm, "getDefaultSensor", "(I)Landroid/hardware/Sensor;", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kSm, "getSensorList", "(I)Ljava/util/List;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(j.newObject("java/util/ArrayList"));
    });
    j.method(kSm, "registerListener", "(Landroid/hardware/SensorEventListener;Landroid/hardware/Sensor;I)Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kSm, "unregisterListener", "(Landroid/hardware/SensorEventListener;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    const std::string kIm = "android/hardware/input/InputManager";
    j.method(kIm, "registerInputDeviceListener", "(Landroid/hardware/input/InputManager$InputDeviceListener;Landroid/os/Handler;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kIm, "unregisterInputDeviceListener", "(Landroid/hardware/input/InputManager$InputDeviceListener;)V",
             [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.lang.System.loadLibrary / load (native APK libraries are preloaded by Library).
    j.method("java/lang/System", "loadLibrary", "(Ljava/lang/String;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        x.proc.logf(LogLevel::Info, "DEX System.loadLibrary(\"%s\")", str(j, a[0]).c_str());
        return JValue(0);
    });
    j.method("java/lang/System", "load", "(Ljava/lang/String;)V", [](Jni& j, Ctx& x, jobject, const auto& a) {
        x.proc.logf(LogLevel::Info, "DEX System.load(\"%s\")", str(j, a[0]).c_str());
        return JValue(0);
    });

    // java.io streams, readers, writers, and extra File methods
    j.superclasses["java/io/BufferedInputStream"] = "java/io/InputStream";
    j.superclasses["java/io/ByteArrayInputStream"] = "java/io/InputStream";
    j.superclasses["java/io/DataInputStream"] = "java/io/InputStream";
    j.superclasses["java/io/BufferedOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/io/ByteArrayOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/io/DataOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/io/FileOutputStream"] = "java/io/OutputStream";
    j.superclasses["java/io/InputStreamReader"] = "java/io/Reader";
    j.superclasses["java/io/BufferedReader"] = "java/io/Reader";
    j.superclasses["java/io/StringReader"] = "java/io/Reader";
    j.superclasses["java/io/OutputStreamWriter"] = "java/io/Writer";
    j.superclasses["java/io/BufferedWriter"] = "java/io/Writer";
    j.superclasses["java/io/StringWriter"] = "java/io/Writer";
    j.superclasses["java/io/PrintWriter"] = "java/io/Writer";

    j.method("java/io/InputStream", "read", "([B)I", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        uint32_t len = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* arr = j.get(jobject(a[0]))) len = uint32_t(arr->data.size());
        }
        const uint32_t mid = j.methodId("java/io/InputStream", "read", "([BII)I", false);
        return j.invoke(x, mid, self, {a[0], 0, len});
    });

    struct MemIn { std::vector<uint8_t> bytes; size_t pos = 0; };
    auto memInOf = [](Jni& j, jobject self) -> std::shared_ptr<MemIn> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* p = std::any_cast<std::shared_ptr<MemIn>>(&o->host)) return *p;
        return nullptr;
    };
    j.method("java/io/ByteArrayInputStream", "<init>", "([B)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        auto m = std::make_shared<MemIn>();
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* arr = j.get(jobject(a[0]))) m->bytes = arr->data;
        if (JObject* o = j.get(self)) o->host = m;
        return JValue(0);
    });
    j.method("java/io/ByteArrayInputStream", "read", "()I", [memInOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto m = memInOf(j, self);
        if (!m || m->pos >= m->bytes.size()) return JValue(uint32_t(-1));
        return JValue(uint32_t(m->bytes[m->pos++]));
    });
    j.method("java/io/ByteArrayInputStream", "read", "([BII)I", [memInOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto m = memInOf(j, self);
        if (!m || m->pos >= m->bytes.size()) return JValue(uint32_t(-1));
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        const size_t n = std::min<size_t>(len, m->bytes.size() - m->pos);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* arr = j.get(jobject(a[0])); arr && size_t(off) + n <= arr->data.size()) {
            std::memcpy(arr->data.data() + off, m->bytes.data() + m->pos, n);
            m->pos += n;
            return JValue(uint32_t(n));
        }
        return JValue(uint32_t(-1));
    });
    j.method("java/io/ByteArrayInputStream", "available", "()I", [memInOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto m = memInOf(j, self);
        return JValue(m && m->pos < m->bytes.size() ? uint32_t(m->bytes.size() - m->pos) : 0);
    });
    j.method("java/io/ByteArrayInputStream", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // BufferedInputStream wraps an underlying InputStream.
    j.method("java/io/BufferedInputStream", "<init>", "(Ljava/io/InputStream;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.in"] = a[0];
        return JValue(0);
    });
    j.method("java/io/BufferedInputStream", "<init>", "(Ljava/io/InputStream;I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.in"] = a[0];
        return JValue(0);
    });
    j.method("java/io/BufferedInputStream", "read", "()I", [](Jni& j, Ctx& x, jobject self, const auto&) {
        jobject in = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) in = jobject(o->fields["hvm.in"]); }
        return in ? j.invoke(x, j.methodId("java/io/InputStream", "read", "()I", false), in, {}) : JValue(uint32_t(-1));
    });
    j.method("java/io/BufferedInputStream", "read", "([BII)I", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        jobject in = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) in = jobject(o->fields["hvm.in"]); }
        return in ? j.invoke(x, j.methodId("java/io/InputStream", "read", "([BII)I", false), in, a) : JValue(uint32_t(-1));
    });
    j.method("java/io/BufferedInputStream", "available", "()I", [](Jni& j, Ctx& x, jobject self, const auto&) {
        jobject in = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) in = jobject(o->fields["hvm.in"]); }
        return in ? j.invoke(x, j.methodId("java/io/InputStream", "available", "()I", false), in, {}) : JValue(0);
    });
    j.method("java/io/BufferedInputStream", "close", "()V", [](Jni& j, Ctx& x, jobject self, const auto&) {
        jobject in = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) in = jobject(o->fields["hvm.in"]); }
        if (in) j.invoke(x, j.methodId("java/io/InputStream", "close", "()V", false), in, {});
        return JValue(0);
    });

    // java.io.OutputStream base methods & BufferedOutputStream
    j.method("java/io/OutputStream", "<init>", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/OutputStream", "write", "(I)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/OutputStream", "write", "([B)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        uint32_t len = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* arr = j.get(jobject(a[0]))) len = uint32_t(arr->data.size()); }
        j.invoke(x, j.methodId("java/io/OutputStream", "write", "([BII)V", false), self, {uint32_t(a[0]), 0u, len});
        return JValue(0);
    });
    j.method("java/io/OutputStream", "write", "([BII)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        std::vector<uint8_t> slice;
        const int32_t off = int32_t(a[1]), len = int32_t(a[2]);
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* arr = j.get(jobject(a[0])); arr && off >= 0 && len > 0 && size_t(off) + size_t(len) <= arr->data.size())
                slice.assign(arr->data.begin() + off, arr->data.begin() + off + len);
        }
        const uint32_t mid = j.methodId("java/io/OutputStream", "write", "(I)V", false);
        for (uint8_t b : slice) j.invoke(x, mid, self, {uint32_t(b)});
        return JValue(0);
    });
    j.method("java/io/OutputStream", "flush", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/OutputStream", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    j.method("java/io/BufferedOutputStream", "<init>", "(Ljava/io/OutputStream;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.out"] = a[0];
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "<init>", "(Ljava/io/OutputStream;I)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["hvm.out"] = a[0];
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "write", "(I)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        jobject out = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) out = jobject(o->fields["hvm.out"]); }
        if (out) j.invoke(x, j.methodId("java/io/OutputStream", "write", "(I)V", false), out, {uint32_t(a[0])});
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "write", "([B)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        jobject out = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) out = jobject(o->fields["hvm.out"]); }
        if (out) j.invoke(x, j.methodId("java/io/OutputStream", "write", "([B)V", false), out, {uint32_t(a[0])});
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "write", "([BII)V", [](Jni& j, Ctx& x, jobject self, const auto& a) {
        jobject out = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) out = jobject(o->fields["hvm.out"]); }
        if (out) j.invoke(x, j.methodId("java/io/OutputStream", "write", "([BII)V", false), out, {uint32_t(a[0]), uint32_t(a[1]), uint32_t(a[2])});
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "flush", "()V", [](Jni& j, Ctx& x, jobject self, const auto&) {
        jobject out = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) out = jobject(o->fields["hvm.out"]); }
        if (out) j.invoke(x, j.methodId("java/io/OutputStream", "flush", "()V", false), out, {});
        return JValue(0);
    });
    j.method("java/io/BufferedOutputStream", "close", "()V", [](Jni& j, Ctx& x, jobject self, const auto&) {
        jobject out = 0;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* o = j.get(self)) out = jobject(o->fields["hvm.out"]); }
        if (out) j.invoke(x, j.methodId("java/io/OutputStream", "close", "()V", false), out, {});
        return JValue(0);
    });

    // ByteArrayOutputStream (maintains protected fields `buf` and `count` per Java SE spec
    // so subclasses such as libGDX's OptimizedByteArrayOutputStream can read them directly)
    auto baosInit = [](Jni& j, jobject self, int32_t cap) {
        const uint32_t size = cap > 0 ? uint32_t(cap) : 32u;
        const jobject buf = j.newArray('B', size);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) {
            o->fields["buf"] = buf;
            o->fields["count"] = 0;
        }
    };
    auto baosAppend = [](Jni& j, jobject self, const uint8_t* src, size_t len) {
        if (!len) return;
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return;
        jobject buf = jobject(o->fields["buf"]);
        size_t count = size_t(uint32_t(o->fields["count"]));
        JObject* arr = j.get(buf);
        if (!arr || count + len > arr->data.size()) {
            const size_t curCap = arr ? arr->data.size() : 0;
            size_t newCap = std::max<size_t>(curCap ? curCap * 2 : 32, count + len);
            const jobject next = j.newArray('B', uint32_t(newCap));
            JObject* nextArr = j.get(next);
            if (arr && nextArr && count > 0)
                std::memcpy(nextArr->data.data(), arr->data.data(), std::min(count, arr->data.size()));
            o->fields["buf"] = next;
            arr = nextArr;
        }
        if (arr && count + len <= arr->data.size()) {
            std::memcpy(arr->data.data() + count, src, len);
            o->fields["count"] = uint64_t(count + len);
        }
    };
    j.method("java/io/ByteArrayOutputStream", "<init>", "()V", [baosInit](Jni& j, Ctx&, jobject self, const auto&) {
        baosInit(j, self, 32);
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "<init>", "(I)V", [baosInit](Jni& j, Ctx&, jobject self, const auto& a) {
        baosInit(j, self, int32_t(a[0]));
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "write", "(I)V", [baosAppend](Jni& j, Ctx&, jobject self, const auto& a) {
        const uint8_t b = uint8_t(a[0]);
        baosAppend(j, self, &b, 1);
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "write", "([B)V", [baosAppend](Jni& j, Ctx&, jobject self, const auto& a) {
        std::vector<uint8_t> tmp;
        { std::lock_guard<std::recursive_mutex> lk(j.mu); if (JObject* arr = j.get(jobject(a[0]))) tmp = arr->data; }
        baosAppend(j, self, tmp.data(), tmp.size());
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "write", "([BII)V", [baosAppend](Jni& j, Ctx&, jobject self, const auto& a) {
        const int32_t off = int32_t(a[1]), len = int32_t(a[2]);
        std::vector<uint8_t> tmp;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* arr = j.get(jobject(a[0])); arr && off >= 0 && len > 0 && size_t(off) + size_t(len) <= arr->data.size())
                tmp.assign(arr->data.begin() + off, arr->data.begin() + off + len);
        }
        baosAppend(j, self, tmp.data(), tmp.size());
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "reset", "()V", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->fields["count"] = 0;
        return JValue(0);
    });
    j.method("java/io/ByteArrayOutputStream", "size", "()I", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        return JValue(o ? uint32_t(o->fields["count"]) : 0u);
    });
    j.method("java/io/ByteArrayOutputStream", "toByteArray", "()[B", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::vector<uint8_t> out;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) {
                const size_t n = size_t(uint32_t(o->fields["count"]));
                if (JObject* buf = j.get(jobject(o->fields["buf"])); buf && n > 0)
                    out.assign(buf->data.begin(), buf->data.begin() + std::min(n, buf->data.size()));
            }
        }
        const jobject arr = j.newArray('B', uint32_t(out.size()));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (!out.empty()) if (JObject* o = j.get(arr)) std::memcpy(o->data.data(), out.data(), out.size());
        return JValue(arr);
    });
    j.method("java/io/ByteArrayOutputStream", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.io.File extra methods
    const std::string kFile = "java/io/File";
    auto fpath = [](Jni& j, jobject f) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(f);
        return o ? o->str : std::string();
    };
    j.method(kFile, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        const std::string path = str(j, a[0]) + "/" + str(j, a[1]);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->str = path;
        return JValue(0);
    });
    j.method(kFile, "canWrite", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(1); });
    j.method(kFile, "getParent", "()Ljava/lang/String;", [fpath](Jni& j, Ctx&, jobject self, const auto&) {
        const std::string parent = fs::path(fpath(j, self)).parent_path().string();
        return JValue(parent.empty() ? 0 : j.newString(parent));
    });
    j.method(kFile, "getParentFile", "()Ljava/io/File;", [fpath](Jni& j, Ctx&, jobject self, const auto&) {
        const std::string parent = fs::path(fpath(j, self)).parent_path().string();
        return JValue(parent.empty() ? 0 : newFileObj(j, parent));
    });
    j.method(kFile, "delete", "()Z", [&p, fpath](Jni& j, Ctx&, jobject self, const auto&) {
        std::error_code ec;
        return JValue(fs::remove(p.hostPath(fpath(j, self)), ec) ? 1 : 0);
    });
    j.method(kFile, "renameTo", "(Ljava/io/File;)Z", [&p, fpath](Jni& j, Ctx&, jobject self, const auto& a) {
        std::error_code ec;
        fs::rename(p.hostPath(fpath(j, self)), p.hostPath(fpath(j, jobject(a[0]))), ec);
        return JValue(ec ? 0 : 1);
    });
    j.method(kFile, "lastModified", "()J", [&p, fpath](Jni& j, Ctx&, jobject self, const auto&) {
        std::error_code ec;
        const auto t = fs::last_write_time(p.hostPath(fpath(j, self)), ec);
        if (ec) return JValue(0);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
        return JValue(uint64_t(ms));
    });
    j.method(kFile, "list", "()[Ljava/lang/String;", [&p, fpath](Jni& j, Ctx&, jobject self, const auto&) {
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(p.hostPath(fpath(j, self)), ec))
            names.push_back(e.path().filename().string());
        if (ec) return JValue(0);
        const jobject arr = j.newArray('L', uint32_t(names.size()));
        for (size_t i = 0; i < names.size(); ++i) {
            const jobject s = j.newString(names[i]);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) o->refs[i] = s;
        }
        return JValue(arr);
    });
    j.method(kFile, "listFiles", "()[Ljava/io/File;", [&p, fpath](Jni& j, Ctx&, jobject self, const auto&) {
        const std::string base = fpath(j, self);
        std::vector<std::string> names;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(p.hostPath(base), ec))
            names.push_back(base + "/" + e.path().filename().string());
        if (ec) return JValue(0);
        const jobject arr = j.newArray('L', uint32_t(names.size()));
        for (size_t i = 0; i < names.size(); ++i) {
            const jobject f = newFileObj(j, names[i]);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(arr)) o->refs[i] = f;
        }
        return JValue(arr);
    });

    // java.io.FileInputStream / FileOutputStream
    struct FileStream { std::FILE* f = nullptr; ~FileStream() { if (f) std::fclose(f); } };
    auto fileStreamOf = [](Jni& j, jobject self) -> std::shared_ptr<FileStream> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self))
            if (auto* p = std::any_cast<std::shared_ptr<FileStream>>(&o->host)) return *p;
        return nullptr;
    };
    j.method("java/io/FileInputStream", "<init>", "(Ljava/io/File;)V", [&p, fpath](Jni& j, Ctx&, jobject self, const auto& a) {
        auto fsObj = std::make_shared<FileStream>();
        fsObj->f = std::fopen(p.hostPath(fpath(j, jobject(a[0]))).c_str(), "rb");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = fsObj;
        return JValue(0);
    });
    j.method("java/io/FileInputStream", "<init>", "(Ljava/io/FileDescriptor;)V", [](Jni& j, Ctx&, jobject self, const auto& a) {
        int fd = -1;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* fdo = j.get(jobject(a[0]))) fd = int(fdo->fields["descriptor"]);
        }
        auto fsObj = std::make_shared<FileStream>();
        if (fd >= 0) {
            const int duped = ::dup(fd);
            if (duped >= 0) fsObj->f = ::fdopen(duped, "rb");
        }
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = fsObj;
        return JValue(0);
    });
    j.method("java/io/FileInputStream", "getChannel", "()Ljava/nio/channels/FileChannel;", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto fsObj = fileStreamOf(j, self);
        const jobject ch = j.newObject("java/nio/channels/FileChannel");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(ch)) o->host = fsObj;
        return JValue(ch);
    });
    j.method("java/nio/channels/FileChannel", "size", "()J", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto fsObj = fileStreamOf(j, self);
        if (!fsObj || !fsObj->f) return JValue(0);
        const long cur = std::ftell(fsObj->f);
        std::fseek(fsObj->f, 0, SEEK_END);
        const long end = std::ftell(fsObj->f);
        std::fseek(fsObj->f, cur, SEEK_SET);
        return JValue(end > 0 ? uint64_t(end) : 0ULL);
    });
    j.method("java/nio/channels/FileChannel", "map",
             "(Ljava/nio/channels/FileChannel$MapMode;JJ)Ljava/nio/MappedByteBuffer;",
             [fileStreamOf](Jni& j, Ctx& x, jobject self, const auto& a) {
        auto fsObj = fileStreamOf(j, self);
        const int64_t pos = int64_t(a[1]);
        const int64_t sz = int64_t(a[2]);
        const uint32_t cap = (sz > 0 && sz <= (64 << 20)) ? uint32_t(sz) : 0u;
        const jobject bb = jobject(j.invoke(x, j.methodId("java/nio/ByteBuffer", "allocateDirect", "(I)Ljava/nio/ByteBuffer;", true), 0, {cap}));
        uint32_t addr = 0;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(bb)) {
                o->cls = "java/nio/MappedByteBuffer";
                addr = uint32_t(o->fields["hvm.address"]);
            }
        }
        if (fsObj && fsObj->f && cap > 0 && addr) {
            const long cur = std::ftell(fsObj->f);
            std::fseek(fsObj->f, long(pos >= 0 ? pos : 0), SEEK_SET);
            (void)std::fread(x.mem.host(addr), 1, cap, fsObj->f);
            std::fseek(fsObj->f, cur, SEEK_SET);
        }
        return JValue(bb);
    });
    j.method("java/nio/channels/FileChannel", "close", "()V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method("java/io/FileInputStream", "read", "()I", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto&) {
        auto s = fileStreamOf(j, self);
        const int c = s && s->f ? std::fgetc(s->f) : EOF;
        return JValue(uint32_t(c == EOF ? -1 : c));
    });
    j.method("java/io/FileInputStream", "read", "([BII)I", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto s = fileStreamOf(j, self);
        if (!s || !s->f) return JValue(uint32_t(-1));
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* arr = j.get(jobject(a[0]));
        if (!arr || size_t(off) + len > arr->data.size()) return JValue(uint32_t(-1));
        const size_t n = std::fread(arr->data.data() + off, 1, len, s->f);
        return JValue(n == 0 && len ? uint32_t(-1) : uint32_t(n));
    });
    j.method("java/io/FileInputStream", "close", "()V", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto s = fileStreamOf(j, self); s && s->f) { std::fclose(s->f); s->f = nullptr; }
        return JValue(0);
    });
    j.method("java/io/FileOutputStream", "<init>", "(Ljava/io/File;)V", [&p, fpath](Jni& j, Ctx&, jobject self, const auto& a) {
        const std::string host = p.hostPath(fpath(j, jobject(a[0])));
        std::error_code ec;
        fs::create_directories(fs::path(host).parent_path(), ec);
        auto fsObj = std::make_shared<FileStream>();
        fsObj->f = std::fopen(host.c_str(), "wb");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = fsObj;
        return JValue(0);
    });
    j.method("java/io/FileOutputStream", "<init>", "(Ljava/io/File;Z)V", [&p, fpath](Jni& j, Ctx&, jobject self, const auto& a) {
        const std::string host = p.hostPath(fpath(j, jobject(a[0])));
        std::error_code ec;
        fs::create_directories(fs::path(host).parent_path(), ec);
        auto fsObj = std::make_shared<FileStream>();
        fsObj->f = std::fopen(host.c_str(), (a[1] & 1) ? "ab" : "wb");
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(self)) o->host = fsObj;
        return JValue(0);
    });
    j.method("java/io/FileOutputStream", "write", "([B)V", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto s = fileStreamOf(j, self);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (s && s->f) if (JObject* arr = j.get(jobject(a[0]))) std::fwrite(arr->data.data(), 1, arr->data.size(), s->f);
        return JValue(0);
    });
    j.method("java/io/FileOutputStream", "write", "([BII)V", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto& a) {
        auto s = fileStreamOf(j, self);
        const uint32_t off = uint32_t(int32_t(a[1])), len = uint32_t(int32_t(a[2]));
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (s && s->f) if (JObject* arr = j.get(jobject(a[0])); arr && size_t(off) + len <= arr->data.size())
            std::fwrite(arr->data.data() + off, 1, len, s->f);
        return JValue(0);
    });
    j.method("java/io/FileOutputStream", "close", "()V", [fileStreamOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto s = fileStreamOf(j, self); s && s->f) { std::fclose(s->f); s->f = nullptr; }
        return JValue(0);
    });

    // java.util.Objects & java.util.logging.Logger
    j.method("java/util/Objects", "equals", "(Ljava/lang/Object;Ljava/lang/Object;)Z", [](Jni& j, Ctx& x, jobject, const auto& a) {
        if (a[0] == a[1]) return JValue(1);
        if (!a[0] || !a[1]) return JValue(0);
        return j.invoke(x, j.methodId("java/lang/Object", "equals", "(Ljava/lang/Object;)Z", false), jobject(a[0]), {a[1]});
    });
    j.method("java/util/logging/Logger", "getLogger", "(Ljava/lang/String;)Ljava/util/logging/Logger;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "java/util/logging/Logger"));
    });
    j.method("java/util/logging/Logger", "log", "(Ljava/util/logging/Level;Ljava/lang/String;Ljava/lang/Throwable;)V", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });

    // java.lang.Thread
    struct ThreadState {
        std::mutex mu;
        std::condition_variable cv;
        bool started = false;
        bool alive = false;
        bool finished = false;
        bool interrupted = false;
        bool daemon = false;
        int32_t priority = 5;
        std::string name = "Thread";
        jobject runnable = 0;
    };
    auto threadStateOf = [](Jni& j, jobject self) -> std::shared_ptr<ThreadState> {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (!o) return nullptr;
        if (auto* sp = std::any_cast<std::shared_ptr<ThreadState>>(&o->host)) return *sp;
        auto st = std::make_shared<ThreadState>();
        o->host = st;
        return st;
    };
    const std::string kThread = "java/lang/Thread";
    j.method(kThread, "<init>", "()V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        threadStateOf(j, self);
        return JValue(0);
    });
    j.method(kThread, "<init>", "(Ljava/lang/String;)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->name = str(j, a[0]);
        }
        return JValue(0);
    });
    j.method(kThread, "<init>", "(Ljava/lang/Runnable;)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->runnable = jobject(a[0]);
        }
        return JValue(0);
    });
    j.method(kThread, "<init>", "(Ljava/lang/Runnable;Ljava/lang/String;)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->runnable = jobject(a[0]);
            st->name = str(j, a[1]);
        }
        return JValue(0);
    });
    j.method(kThread, "setName", "(Ljava/lang/String;)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->name = str(j, a[0]);
        }
        return JValue(0);
    });
    j.method(kThread, "getName", "()Ljava/lang/String;", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        std::string nm = "Thread";
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            nm = st->name;
        }
        return JValue(j.newString(nm));
    });
    j.method(kThread, "setDaemon", "(Z)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->daemon = (a[0] & 1) != 0;
        }
        return JValue(0);
    });
    j.method(kThread, "isDaemon", "()Z", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            return JValue(st->daemon ? 1 : 0);
        }
        return JValue(0);
    });
    j.method(kThread, "setPriority", "(I)V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto& a) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->priority = int32_t(a[0]);
        }
        return JValue(0);
    });
    j.method(kThread, "getPriority", "()I", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            return JValue(uint32_t(st->priority));
        }
        return JValue(5);
    });
    j.method(kThread, "interrupt", "()V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->interrupted = true;
        }
        return JValue(0);
    });
    j.method(kThread, "isInterrupted", "()Z", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            return JValue(st->interrupted ? 1 : 0);
        }
        return JValue(0);
    });
    j.method(kThread, "interrupted", "()Z", [](Jni&, Ctx&, jobject, const auto&) { return JValue(0); });
    j.method(kThread, "isAlive", "()Z", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::lock_guard<std::mutex> lk(st->mu);
            return JValue(st->alive ? 1 : 0);
        }
        return JValue(0);
    });
    j.method(kThread, "currentThread", "()Ljava/lang/Thread;", [threadStateOf](Jni& j, Ctx&, jobject, const auto&) {
        const jobject cur = singleton(j, "java/lang/Thread");
        if (auto st = threadStateOf(j, cur)) {
            std::lock_guard<std::mutex> lk(st->mu);
            st->name = "main";
            st->alive = true;
        }
        return JValue(cur);
    });
    j.method(kThread, "getStackTrace", "()[Ljava/lang/StackTraceElement;", [](Jni& j, Ctx&, jobject, const auto&) {
        const jobject arr = j.newArray('L', 0);
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        if (JObject* o = j.get(arr)) o->cls = "[Ljava/lang/StackTraceElement;";
        return JValue(arr);
    });
    j.method(kThread, "start", "()V", [threadStateOf](Jni& j, Ctx& x, jobject self, const auto&) {
        std::string cls;
        {
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(self)) cls = o->cls;
        }
        auto st = threadStateOf(j, self);
        if (!st) return JValue(0);
        jobject target = 0;
        {
            std::lock_guard<std::mutex> lk(st->mu);
            if (st->started) return JValue(0);
            st->started = true;
            if (cls == "android/os/HandlerThread") {
                st->alive = true;
                return JValue(0);
            }
            target = st->runnable;
            if (!target) {
                st->finished = true;
                return JValue(0);
            }
            st->alive = true;
        }
        Process* proc = &x.proc;
        Dex::guestScheduler(*proc)([proc, st, target] {
            try {
                auto& jniEnv = jni(*proc);
                Ctx wx{*proc, proc->currentThread().cpu, proc->mem()};
                std::string rcls;
                {
                    std::lock_guard<std::recursive_mutex> lk(jniEnv.mu);
                    if (JObject* ro = jniEnv.get(target)) rcls = ro->cls;
                }
                JValue ignored = 0;
                if (!rcls.empty()) {
                    Dex::invokeFromJni(jniEnv, wx, {rcls, "run", "()V", false}, target, {}, rcls, false, ignored);
                }
            } catch (...) {}
            std::lock_guard<std::mutex> lk(st->mu);
            st->alive = false;
            st->finished = true;
            st->cv.notify_all();
        });
        return JValue(0);
    });
    j.method(kThread, "join", "()V", [threadStateOf](Jni& j, Ctx&, jobject self, const auto&) {
        if (auto st = threadStateOf(j, self)) {
            std::unique_lock<std::mutex> lk(st->mu);
            if (st->started && st->runnable) st->cv.wait(lk, [&] { return st->finished; });
        }
        return JValue(0);
    });

    j.method("android/view/LayoutInflater", "getContext", "()Landroid/content/Context;", [](Jni& j, Ctx&, jobject self, const auto&) {
        std::lock_guard<std::recursive_mutex> lk(j.mu);
        JObject* o = j.get(self);
        if (o && o->fields["hvm.context"]) return JValue(o->fields["hvm.context"]);
        return JValue(singleton(j, "android/app/Activity"));
    });
    j.method("android/os/VibratorManager", "getDefaultVibrator", "()Landroid/os/Vibrator;", [](Jni& j, Ctx&, jobject, const auto&) {
        return JValue(singleton(j, "android/os/Vibrator"));
    });

    // java.nio.channels.FileChannel$MapMode constants
    {
        const std::string kMapMode = "java/nio/channels/FileChannel$MapMode";
        auto makeMode = [&](const char* field) {
            const jobject obj = j.newObject(kMapMode);
            std::lock_guard<std::recursive_mutex> lk(j.mu);
            if (JObject* o = j.get(obj)) { o->permanent = true; o->str = field; }
            j.staticFields[kMapMode + "." + field] = obj;
        };
        makeMode("READ_ONLY");
        makeMode("READ_WRITE");
        makeMode("PRIVATE");
        j.superclasses["java/nio/MappedByteBuffer"] = "java/nio/ByteBuffer";
    }

    // java.nio.* buffers
    registerNioBuffers(j);
}

} // namespace Hydrogen::Runtime::Android
