// HydrogenVM — CommonCrypto digests (MD5, SHA-1, SHA-256) and HMAC.
//
// The CC_*_CTX structs the app allocates are big enough to hold our state
// directly (MD5 92 bytes, SHA1 96, SHA256 104); we keep it there, copied in and
// out with memcpy since guest memory may not be aligned for host types.
// CCCrypt and the CCCryptor API are forwarded to the host platform's crypto
// library (HostCrypto.cpp); this file implements no cipher.
#include "runtime/Process.hpp"
#include "runtime/libc/HostCrypto.hpp"
#include <mutex>
#include <unordered_map>
#include <cstring>
#include <string>
#include <vector>

namespace Hydrogen::Runtime {
namespace {

inline uint32_t rol(uint32_t v, int n) { return (v << n) | (v >> (32 - n)); }
inline uint32_t ror(uint32_t v, int n) { return (v >> n) | (v << (32 - n)); }

struct Md5 {
    uint32_t h[4];
    uint32_t lo, hi;      // byte count
    uint8_t buf[64];
    void init() { h[0] = 0x67452301; h[1] = 0xefcdab89; h[2] = 0x98badcfe; h[3] = 0x10325476; lo = hi = 0; }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
            0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
            0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
            0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
            0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
            0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const int R[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                  5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
                                  6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i) m[i] = uint32_t(p[i * 4]) | uint32_t(p[i * 4 + 1]) << 8 | uint32_t(p[i * 4 + 2]) << 16 | uint32_t(p[i * 4 + 3]) << 24;
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (int i = 0; i < 64; ++i) {
            uint32_t f; int g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
            else { f = c ^ (b | ~d); g = (7 * i) & 15; }
            const uint32_t t = d; d = c; c = b;
            b = b + rol(a + f + K[i] + m[g], R[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    static constexpr bool kBigEndian = false;
    static constexpr int kWords = 4;
};

struct Sha1 {
    uint32_t h[5];
    uint32_t lo, hi;
    uint8_t buf[64];
    void init() { h[0] = 0x67452301; h[1] = 0xEFCDAB89; h[2] = 0x98BADCFE; h[3] = 0x10325476; h[4] = 0xC3D2E1F0; lo = hi = 0; }
    void block(const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    static constexpr bool kBigEndian = true;
    static constexpr int kWords = 5;
};

struct Sha256 {
    uint32_t h[8];
    uint32_t lo, hi;
    uint8_t buf[64];
    void init() {
        const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
        std::memcpy(h, iv, sizeof iv);
        lo = hi = 0;
    }
    void block(const uint8_t* p) {
        static const uint32_t K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
            0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
            0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
            0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
            0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
            0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = uint32_t(p[i * 4]) << 24 | uint32_t(p[i * 4 + 1]) << 16 | uint32_t(p[i * 4 + 2]) << 8 | p[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = ror(e, 6) ^ ror(e, 11) ^ ror(e, 25), ch = (e & f) ^ (~e & g);
            const uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            const uint32_t S0 = ror(a, 2) ^ ror(a, 13) ^ ror(a, 22), maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    static constexpr bool kBigEndian = true;
    static constexpr int kWords = 8;
};

template <typename H> void update(H& s, const uint8_t* p, size_t n) {
    size_t used = s.lo & 63;
    const uint32_t old = s.lo;
    s.lo += uint32_t(n);
    if (s.lo < old) ++s.hi;
    s.hi += uint32_t(uint64_t(n) >> 32);
    while (n) {
        const size_t k = std::min(n, 64 - used);
        std::memcpy(s.buf + used, p, k);
        used += k; p += k; n -= k;
        if (used == 64) { s.block(s.buf); used = 0; }
    }
}
template <typename H> void finish(H& s, uint8_t* out) {
    const uint64_t bits = ((uint64_t(s.hi) << 32) | s.lo) * 8;
    uint8_t pad[72] = {0x80};
    const size_t used = s.lo & 63;
    const size_t padLen = (used < 56 ? 56 : 120) - used;
    uint8_t len[8];
    for (int i = 0; i < 8; ++i) len[i] = H::kBigEndian ? uint8_t(bits >> (56 - 8 * i)) : uint8_t(bits >> (8 * i));
    update(s, pad, padLen);
    update(s, len, 8);
    for (int i = 0; i < H::kWords; ++i)
        for (int b = 0; b < 4; ++b)
            out[i * 4 + b] = H::kBigEndian ? uint8_t(s.h[i] >> (24 - 8 * b)) : uint8_t(s.h[i] >> (8 * b));
}
template <typename H> std::vector<uint8_t> digest(const uint8_t* p, size_t n) {
    H s;
    s.init();
    update(s, p, n);
    std::vector<uint8_t> out(H::kWords * 4);
    finish(s, out.data());
    return out;
}

template <typename H> H load(Ctx& x, GuestPtr c) { H s; std::memcpy(&s, x.mem.host(c), sizeof s); return s; }
template <typename H> void store(Ctx& x, GuestPtr c, const H& s) { std::memcpy(x.mem.host(c), &s, sizeof s); }

template <typename H> int32_t cc_init(Ctx& x, GuestPtr c) { H s; s.init(); store(x, c, s); return 1; }
template <typename H> int32_t cc_update(Ctx& x, GuestPtr c, GuestPtr data, uint32_t n) {
    H s = load<H>(x, c);
    if (n) update(s, x.mem.host(data), n);
    store(x, c, s);
    return 1;
}
template <typename H> int32_t cc_final(Ctx& x, GuestPtr md, GuestPtr c) {
    H s = load<H>(x, c);
    uint8_t out[32];
    finish(s, out);
    x.mem.copyIn(md, out, H::kWords * 4);
    return 1;
}
template <typename H> uint32_t cc_oneShot(Ctx& x, GuestPtr data, uint32_t n, GuestPtr md) {
    const auto d = digest<H>(n ? x.mem.host(data) : nullptr, n);
    x.mem.copyIn(md, d.data(), d.size());
    return md;
}

template <typename H> std::vector<uint8_t> hmac(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t n) {
    std::vector<uint8_t> k(key, key + keyLen);
    if (k.size() > 64) k = digest<H>(k.data(), k.size());
    k.resize(64, 0);
    std::vector<uint8_t> ipad(64), opad(64);
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    H s;
    s.init();
    update(s, ipad.data(), 64);
    update(s, data, n);
    std::vector<uint8_t> inner(H::kWords * 4);
    finish(s, inner.data());
    s.init();
    update(s, opad.data(), 64);
    update(s, inner.data(), inner.size());
    std::vector<uint8_t> out(H::kWords * 4);
    finish(s, out.data());
    return out;
}
// Incremental HMAC. CCHmacContext is 384 bytes; we keep {alg, inner hash state, outer hash state} in it.
template <typename H> struct HmacCtx { uint32_t alg; uint32_t pad; H inner, outer; };
static_assert(sizeof(HmacCtx<Sha256>) <= 384, "CCHmacContext is 384 bytes");
template <typename H> void hmacInit(Ctx& x, GuestPtr c, uint32_t alg, const uint8_t* key, size_t keyLen) {
    std::vector<uint8_t> k(key, key + keyLen);
    if (k.size() > 64) k = digest<H>(k.data(), k.size());
    k.resize(64, 0);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5c; }
    HmacCtx<H> h{};
    h.alg = alg;
    h.inner.init(); update(h.inner, ipad, 64);
    h.outer.init(); update(h.outer, opad, 64);
    std::memcpy(x.mem.host(c), &h, sizeof h);
}
template <typename H> void hmacUpdate(Ctx& x, GuestPtr c, const uint8_t* p, size_t n) {
    HmacCtx<H> h;
    std::memcpy(&h, x.mem.host(c), sizeof h);
    if (n) update(h.inner, p, n);
    std::memcpy(x.mem.host(c), &h, sizeof h);
}
template <typename H> void hmacFinal(Ctx& x, GuestPtr c, GuestPtr out) {
    HmacCtx<H> h;
    std::memcpy(&h, x.mem.host(c), sizeof h);
    uint8_t inner[32], mac[32];
    finish(h.inner, inner);
    update(h.outer, inner, H::kWords * 4);
    finish(h.outer, mac);
    x.mem.copyIn(out, mac, H::kWords * 4);
}
void cc_hmacInit(Ctx& x, GuestPtr c, uint32_t alg, GuestPtr key, uint32_t keyLen) {
    const uint8_t* k = keyLen ? x.mem.host(key) : nullptr;
    if (alg == 0) hmacInit<Sha1>(x, c, alg, k, keyLen);
    else if (alg == 1) hmacInit<Md5>(x, c, alg, k, keyLen);
    else if (alg == 2) hmacInit<Sha256>(x, c, alg, k, keyLen);
    else throw std::runtime_error("CCHmacInit: algorithm " + std::to_string(alg) + " isn't supported");
}
void cc_hmacUpdate(Ctx& x, GuestPtr c, GuestPtr data, uint32_t n) {
    const uint32_t alg = x.mem.read<uint32_t>(c);
    const uint8_t* d = n ? x.mem.host(data) : nullptr;
    if (alg == 0) hmacUpdate<Sha1>(x, c, d, n); else if (alg == 1) hmacUpdate<Md5>(x, c, d, n); else hmacUpdate<Sha256>(x, c, d, n);
}
void cc_hmacFinal(Ctx& x, GuestPtr c, GuestPtr out) {
    const uint32_t alg = x.mem.read<uint32_t>(c);
    if (alg == 0) hmacFinal<Sha1>(x, c, out); else if (alg == 1) hmacFinal<Md5>(x, c, out); else hmacFinal<Sha256>(x, c, out);
}

// CCHmac(alg, key, keyLength, data, dataLength, macOut): kCCHmacAlgSHA1 0, MD5 1, SHA256 2.
void cc_hmac(Ctx& x) {
    const uint32_t alg = x.cpu.r[0], key = x.cpu.r[1], keyLen = x.cpu.r[2], data = x.cpu.r[3];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t n = x.mem.read<uint32_t>(sp), out = x.mem.read<uint32_t>(sp + 4);
    const uint8_t* k = keyLen ? x.mem.host(key) : nullptr;
    const uint8_t* d = n ? x.mem.host(data) : nullptr;
    std::vector<uint8_t> mac;
    if (alg == 0) mac = hmac<Sha1>(k, keyLen, d, n);
    else if (alg == 1) mac = hmac<Md5>(k, keyLen, d, n);
    else if (alg == 2) mac = hmac<Sha256>(k, keyLen, d, n);
    else throw std::runtime_error("CCHmac: algorithm " + std::to_string(alg) + " isn't supported");
    x.mem.copyIn(out, mac.data(), mac.size());
}
const uint8_t* hostPtr(Ctx& x, GuestPtr p, size_t n) { return (p && n) ? x.mem.host(p) : nullptr; }

void warnUnavailable(Ctx& x) {
    static const char key = 0;
    uint32_t& warned = x.proc.processSlot(&key);
    if (warned) return;
    warned = 1;
    x.proc.log(LogLevel::Warning, "CCCrypt: no host crypto library in this build; the app gets kCCUnimplemented");
}

// CCCrypt(op, alg, options, key, keyLength, iv, dataIn, dataInLength, dataOut,
//         dataOutAvailable, dataOutMoved): r0-r3, then seven words on the stack.
void cc_crypt(Ctx& x) {
    const uint32_t op = x.cpu.r[0], alg = x.cpu.r[1], options = x.cpu.r[2], key = x.cpu.r[3];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t keyLen = x.mem.read<uint32_t>(sp), iv = x.mem.read<uint32_t>(sp + 4);
    const uint32_t in = x.mem.read<uint32_t>(sp + 8), inLen = x.mem.read<uint32_t>(sp + 12);
    const uint32_t out = x.mem.read<uint32_t>(sp + 16), outAvail = x.mem.read<uint32_t>(sp + 20);
    const uint32_t movedPtr = x.mem.read<uint32_t>(sp + 24);
    if (!HostCrypto::available()) warnUnavailable(x);
    std::vector<uint8_t> result(static_cast<size_t>(outAvail));
    size_t moved = 0;
    const int32_t st = HostCrypto::oneShot(op, alg, options, hostPtr(x, key, keyLen), keyLen, hostPtr(x, iv, 1),
                                           hostPtr(x, in, inLen), inLen, result.data(), outAvail, &moved);
    if (st == HostCrypto::kSuccess && moved) x.mem.copyIn(out, result.data(), moved);
    if (movedPtr) x.mem.write<uint32_t>(movedPtr, uint32_t(moved));
    x.cpu.r[0] = uint32_t(st);
}

// CCCryptorRef handles: a small guest block per cryptor, mapped to the host object.
struct Cryptors {
    explicit Cryptors(Process&) {}
    ~Cryptors() { for (auto& [h, c] : live) HostCrypto::release(c); }
    std::mutex mu;
    std::unordered_map<uint32_t, HostCrypto::Cryptor*> live;
};
HostCrypto::Cryptor* cryptorOf(Ctx& x, uint32_t h) {
    auto& s = x.proc.service<Cryptors>();
    std::lock_guard<std::mutex> lk(s.mu);
    auto it = s.live.find(h);
    return it == s.live.end() ? nullptr : it->second;
}

// CCCryptorCreate(op, alg, options, key, keyLength, iv, cryptorRef*)
void cc_cryptorCreate(Ctx& x) {
    const uint32_t op = x.cpu.r[0], alg = x.cpu.r[1], options = x.cpu.r[2], key = x.cpu.r[3];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t keyLen = x.mem.read<uint32_t>(sp), iv = x.mem.read<uint32_t>(sp + 4), outRef = x.mem.read<uint32_t>(sp + 8);
    if (!HostCrypto::available()) warnUnavailable(x);
    HostCrypto::Cryptor* c = nullptr;
    const int32_t st = HostCrypto::create(op, alg, options, hostPtr(x, key, keyLen), keyLen, hostPtr(x, iv, 1), &c);
    uint32_t h = 0;
    if (st == HostCrypto::kSuccess) {
        h = x.proc.heap().allocZeroed(16);
        auto& s = x.proc.service<Cryptors>();
        std::lock_guard<std::mutex> lk(s.mu);
        s.live[h] = c;
    }
    if (outRef) x.mem.write<uint32_t>(outRef, h);
    x.cpu.r[0] = uint32_t(st);
}
// CCCryptorUpdate(ref, dataIn, dataInLength, dataOut, dataOutAvailable, dataOutMoved*)
void cc_cryptorUpdate(Ctx& x) {
    const uint32_t ref = x.cpu.r[0], in = x.cpu.r[1], inLen = x.cpu.r[2], out = x.cpu.r[3];
    const uint32_t sp = x.cpu.r[CPU::kSP];
    const uint32_t outAvail = x.mem.read<uint32_t>(sp), movedPtr = x.mem.read<uint32_t>(sp + 4);
    HostCrypto::Cryptor* c = cryptorOf(x, ref);
    size_t moved = 0;
    int32_t st = HostCrypto::kParamError;
    if (c) {
        std::vector<uint8_t> result(static_cast<size_t>(outAvail));
        st = HostCrypto::update(c, hostPtr(x, in, inLen), inLen, result.data(), outAvail, &moved);
        if (st == HostCrypto::kSuccess && moved) x.mem.copyIn(out, result.data(), moved);
    }
    if (movedPtr) x.mem.write<uint32_t>(movedPtr, uint32_t(moved));
    x.cpu.r[0] = uint32_t(st);
}
// CCCryptorFinal(ref, dataOut, dataOutAvailable, dataOutMoved*)
void cc_cryptorFinal(Ctx& x) {
    const uint32_t ref = x.cpu.r[0], out = x.cpu.r[1], outAvail = x.cpu.r[2], movedPtr = x.cpu.r[3];
    HostCrypto::Cryptor* c = cryptorOf(x, ref);
    size_t moved = 0;
    int32_t st = HostCrypto::kParamError;
    if (c) {
        std::vector<uint8_t> result(size_t(outAvail) + 64);
        st = HostCrypto::final(c, result.data(), outAvail, &moved);
        if (st == HostCrypto::kSuccess && moved) x.mem.copyIn(out, result.data(), moved);
    }
    if (movedPtr) x.mem.write<uint32_t>(movedPtr, uint32_t(moved));
    x.cpu.r[0] = uint32_t(st);
}
int32_t cc_cryptorRelease(Ctx& x, uint32_t ref) {
    HostCrypto::Cryptor* c = nullptr;
    {
        auto& s = x.proc.service<Cryptors>();
        std::lock_guard<std::mutex> lk(s.mu);
        auto it = s.live.find(ref);
        if (it == s.live.end()) return HostCrypto::kSuccess;
        c = it->second;
        s.live.erase(it);
    }
    HostCrypto::release(c);
    x.proc.heap().free(ref);
    return HostCrypto::kSuccess;
}
int32_t cc_cryptorReset(Ctx& x, uint32_t ref, GuestPtr iv) {
    HostCrypto::Cryptor* c = cryptorOf(x, ref);
    return c ? HostCrypto::reset(c, hostPtr(x, iv, 1)) : HostCrypto::kParamError;
}
uint32_t cc_cryptorGetOutputLength(Ctx& x, uint32_t ref, uint32_t inLen, uint32_t final) {
    HostCrypto::Cryptor* c = cryptorOf(x, ref);
    return c ? uint32_t(HostCrypto::outputLength(c, inLen, (final & 0xff) != 0)) : 0;
}

} // namespace

void registerCommonCrypto(Process& p) {
    p.def<&cc_init<Md5>>("_CC_MD5_Init");
    p.def<&cc_update<Md5>>("_CC_MD5_Update");
    p.def<&cc_final<Md5>>("_CC_MD5_Final");
    p.def<&cc_oneShot<Md5>>("_CC_MD5");
    p.def<&cc_init<Sha1>>("_CC_SHA1_Init");
    p.def<&cc_update<Sha1>>("_CC_SHA1_Update");
    p.def<&cc_final<Sha1>>("_CC_SHA1_Final");
    p.def<&cc_oneShot<Sha1>>("_CC_SHA1");
    p.def<&cc_init<Sha256>>("_CC_SHA256_Init");
    p.def<&cc_update<Sha256>>("_CC_SHA256_Update");
    p.def<&cc_final<Sha256>>("_CC_SHA256_Final");
    p.def<&cc_oneShot<Sha256>>("_CC_SHA256");
    p.defineFunction("_CCHmac", cc_hmac);
    p.def<&cc_hmacInit>("_CCHmacInit");
    p.def<&cc_hmacUpdate>("_CCHmacUpdate");
    p.def<&cc_hmacFinal>("_CCHmacFinal");
    p.defineFunction("_CCCrypt", cc_crypt);
    p.defineFunction("_CCCryptorCreate", cc_cryptorCreate);
    p.defineFunction("_CCCryptorUpdate", cc_cryptorUpdate);
    p.defineFunction("_CCCryptorFinal", cc_cryptorFinal);
    p.def<&cc_cryptorRelease>("_CCCryptorRelease");
    p.def<&cc_cryptorReset>("_CCCryptorReset");
    p.def<&cc_cryptorGetOutputLength>("_CCCryptorGetOutputLength");
}

// For tests: HMAC-SHA256 of `data` with `key` (RFC 4231 test case 2 is key "Jefe").
std::vector<uint8_t> hvmHmacSha256(const std::string& key, const std::string& data) {
    return hmac<Sha256>(reinterpret_cast<const uint8_t*>(key.data()), key.size(), reinterpret_cast<const uint8_t*>(data.data()), data.size());
}
// For tests.
std::vector<uint8_t> hvmDigest(int alg, const std::string& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(s.data());
    return alg == 0 ? digest<Md5>(p, s.size()) : alg == 1 ? digest<Sha1>(p, s.size()) : digest<Sha256>(p, s.size());
}

} // namespace Hydrogen::Runtime
