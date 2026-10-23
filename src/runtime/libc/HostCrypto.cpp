// HydrogenVM — CommonCrypto ciphers forwarded to the host platform's crypto library.
// See HostCrypto.hpp. No cipher is implemented here: Apple hosts call the system
// CommonCrypto, Linux builds with OpenSSL call EVP, anything else reports
// kCCUnimplemented.
#include "runtime/libc/HostCrypto.hpp"

#if defined(__APPLE__)
#include <CommonCrypto/CommonCryptor.h>
#elif defined(HVM_HAVE_OPENSSL)
#include <openssl/evp.h>
#include <openssl/provider.h>
#include <mutex>
#include <string>
#include <vector>
#endif

namespace Hydrogen::Runtime::HostCrypto {

#if defined(__APPLE__)

struct Cryptor { CCCryptorRef ref = nullptr; };

bool available() { return true; }

int32_t create(uint32_t op, uint32_t alg, uint32_t options, const uint8_t* key, size_t keyLength,
               const uint8_t* iv, Cryptor** out) {
    *out = nullptr;
    CCCryptorRef ref = nullptr;
    const CCCryptorStatus st = CCCryptorCreate(op, alg, options, key, keyLength, iv, &ref);
    if (st != kCCSuccess) return st;
    *out = new Cryptor{ref};
    return kSuccess;
}
int32_t update(Cryptor* c, const uint8_t* in, size_t inLength, uint8_t* out, size_t outAvailable, size_t* moved) {
    return CCCryptorUpdate(c->ref, in, inLength, out, outAvailable, moved);
}
int32_t final(Cryptor* c, uint8_t* out, size_t outAvailable, size_t* moved) {
    return CCCryptorFinal(c->ref, out, outAvailable, moved);
}
int32_t reset(Cryptor* c, const uint8_t* iv) { return CCCryptorReset(c->ref, iv); }
size_t outputLength(Cryptor* c, size_t inLength, bool final) { return CCCryptorGetOutputLength(c->ref, inLength, final); }
void release(Cryptor* c) {
    if (!c) return;
    CCCryptorRelease(c->ref);
    delete c;
}

#elif defined(HVM_HAVE_OPENSSL)

namespace {

// OpenSSL 3 keeps DES, CAST, RC4, RC2 and Blowfish in the legacy provider.
void loadProviders() {
    static std::once_flag once;
    std::call_once(once, [] {
        OSSL_PROVIDER_load(nullptr, "default");
        OSSL_PROVIDER_load(nullptr, "legacy");
    });
}

// The EVP cipher name for a CommonCrypto algorithm, key length and mode.
// Returns an empty string when the combination isn't valid.
std::string cipherName(uint32_t alg, size_t keyLength, bool ecb, bool& variableKey, int32_t& error) {
    variableKey = false;
    error = kSuccess;
    const char* mode = ecb ? "-ecb" : "-cbc";
    switch (alg) {
        case kAES:
            if (keyLength != 16 && keyLength != 24 && keyLength != 32) { error = kKeySizeError; return {}; }
            return "aes-" + std::to_string(keyLength * 8) + mode;
        case kDES:
            if (keyLength != 8) { error = kKeySizeError; return {}; }
            return std::string("des") + mode;
        case k3DES:
            if (keyLength != 24) { error = kKeySizeError; return {}; }
            return std::string("des-ede3") + mode;
        case kCAST:
            if (keyLength < 5 || keyLength > 16) { error = kKeySizeError; return {}; }
            variableKey = true;
            return std::string("cast5") + mode;
        case kRC4:
            if (keyLength < 1 || keyLength > 512) { error = kKeySizeError; return {}; }
            variableKey = true;
            return "rc4";
        case kRC2:
            if (keyLength < 1 || keyLength > 128) { error = kKeySizeError; return {}; }
            variableKey = true;
            return std::string("rc2") + mode;
        case kBlowfish:
            if (keyLength < 8 || keyLength > 56) { error = kKeySizeError; return {}; }
            variableKey = true;
            return std::string("bf") + mode;
        default:
            error = kParamError;
            return {};
    }
}

} // namespace

struct Cryptor {
    EVP_CIPHER_CTX* ctx = nullptr;
    EVP_CIPHER* cipher = nullptr;
    bool encrypt = true, padding = false;
    size_t block = 1;
    size_t buffered = 0;   // input bytes held back by the cipher (not yet output)
};

bool available() { return true; }

int32_t create(uint32_t op, uint32_t alg, uint32_t options, const uint8_t* key, size_t keyLength,
               const uint8_t* iv, Cryptor** out) {
    *out = nullptr;
    if (op != kEncrypt && op != kDecrypt) return kParamError;
    if (!key && keyLength) return kParamError;
    loadProviders();
    bool variableKey = false;
    int32_t error = kSuccess;
    const std::string name = cipherName(alg, keyLength, (options & kOptionECBMode) != 0, variableKey, error);
    if (name.empty()) return error;
    EVP_CIPHER* cipher = EVP_CIPHER_fetch(nullptr, name.c_str(), nullptr);
    if (!cipher) return kUnimplemented;
    auto* c = new Cryptor;
    c->cipher = cipher;
    c->ctx = EVP_CIPHER_CTX_new();
    c->encrypt = op == kEncrypt;
    c->padding = (options & kOptionPKCS7Padding) != 0;
    c->block = size_t(EVP_CIPHER_get_block_size(cipher));
    if (c->block == 0) c->block = 1;
    // A null IV means all zeros, as in CommonCrypto.
    std::vector<uint8_t> zeroIv(size_t(EVP_CIPHER_get_iv_length(cipher)), 0);
    const uint8_t* useIv = iv ? iv : (zeroIv.empty() ? nullptr : zeroIv.data());
    bool ok = c->ctx && EVP_CipherInit_ex2(c->ctx, cipher, nullptr, nullptr, c->encrypt ? 1 : 0, nullptr) == 1;
    if (ok && variableKey) ok = EVP_CIPHER_CTX_set_key_length(c->ctx, int(keyLength)) == 1;
    if (ok) ok = EVP_CipherInit_ex2(c->ctx, nullptr, key, useIv, c->encrypt ? 1 : 0, nullptr) == 1;
    if (ok) ok = EVP_CIPHER_CTX_set_padding(c->ctx, c->padding ? 1 : 0) == 1;
    if (!ok) {
        release(c);
        return kParamError;
    }
    *out = c;
    return kSuccess;
}

size_t outputLength(Cryptor* c, size_t inLength, bool final) {
    const size_t total = c->buffered + inLength;
    if (c->block <= 1) return total;
    if (final) {
        if (c->encrypt && c->padding) return (total / c->block + 1) * c->block;
        return total;
    }
    // A padded decrypt holds back its last whole block until final.
    if (!c->encrypt && c->padding && total && total % c->block == 0) return total - c->block;
    return total / c->block * c->block;
}

int32_t update(Cryptor* c, const uint8_t* in, size_t inLength, uint8_t* out, size_t outAvailable, size_t* moved) {
    if (moved) *moved = 0;
    if (!in && inLength) return kParamError;
    const size_t needed = outputLength(c, inLength, false);
    if (outAvailable < needed) {
        if (moved) *moved = needed;
        return kBufferTooSmall;
    }
    // EVP may write up to inLength + block - 1 bytes; stage through a host buffer.
    std::vector<uint8_t> tmp(inLength + c->block);
    int n = 0;
    if (inLength && EVP_CipherUpdate(c->ctx, tmp.data(), &n, in, int(inLength)) != 1) return kParamError;
    if (size_t(n) > outAvailable) return kBufferTooSmall;
    if (n) std::copy(tmp.begin(), tmp.begin() + n, out);
    c->buffered = c->buffered + inLength - size_t(n);
    if (moved) *moved = size_t(n);
    return kSuccess;
}

int32_t final(Cryptor* c, uint8_t* out, size_t outAvailable, size_t* moved) {
    if (moved) *moved = 0;
    if (!c->padding && c->block > 1 && c->buffered % c->block) return kAlignmentError;
    const size_t needed = outputLength(c, 0, true);
    if (outAvailable < needed && !(c->encrypt == false && c->padding)) {
        if (moved) *moved = needed;
        return kBufferTooSmall;
    }
    std::vector<uint8_t> tmp(c->block * 2);
    int n = 0;
    if (EVP_CipherFinal_ex(c->ctx, tmp.data(), &n) != 1) return c->encrypt ? kParamError : kDecodeError;
    if (size_t(n) > outAvailable) {
        if (moved) *moved = size_t(n);
        return kBufferTooSmall;
    }
    if (n) std::copy(tmp.begin(), tmp.begin() + n, out);
    c->buffered = 0;
    if (moved) *moved = size_t(n);
    return kSuccess;
}

int32_t reset(Cryptor* c, const uint8_t* iv) {
    std::vector<uint8_t> zeroIv(size_t(EVP_CIPHER_get_iv_length(c->cipher)), 0);
    const uint8_t* useIv = iv ? iv : (zeroIv.empty() ? nullptr : zeroIv.data());
    if (EVP_CipherInit_ex2(c->ctx, nullptr, nullptr, useIv, -1, nullptr) != 1) return kParamError;
    c->buffered = 0;
    return kSuccess;
}

void release(Cryptor* c) {
    if (!c) return;
    if (c->ctx) EVP_CIPHER_CTX_free(c->ctx);
    if (c->cipher) EVP_CIPHER_free(c->cipher);
    delete c;
}

#else

struct Cryptor {};
bool available() { return false; }
int32_t create(uint32_t, uint32_t, uint32_t, const uint8_t*, size_t, const uint8_t*, Cryptor** out) {
    *out = nullptr;
    return kUnimplemented;
}
int32_t update(Cryptor*, const uint8_t*, size_t, uint8_t*, size_t, size_t* moved) { if (moved) *moved = 0; return kUnimplemented; }
int32_t final(Cryptor*, uint8_t*, size_t, size_t* moved) { if (moved) *moved = 0; return kUnimplemented; }
int32_t reset(Cryptor*, const uint8_t*) { return kUnimplemented; }
size_t outputLength(Cryptor*, size_t inLength, bool) { return inLength; }
void release(Cryptor*) {}

#endif

int32_t oneShot(uint32_t op, uint32_t alg, uint32_t options, const uint8_t* key, size_t keyLength, const uint8_t* iv,
                const uint8_t* in, size_t inLength, uint8_t* out, size_t outAvailable, size_t* moved) {
    if (moved) *moved = 0;
    Cryptor* c = nullptr;
    int32_t st = create(op, alg, options, key, keyLength, iv, &c);
    if (st != kSuccess) return st;
    const size_t needed = outputLength(c, inLength, true);
    // A padded decrypt can produce less than its input; only encryption knows the exact size up front.
    if (outAvailable < needed && !(op == kDecrypt && (options & kOptionPKCS7Padding))) {
        if (moved) *moved = needed;
        release(c);
        return kBufferTooSmall;
    }
    size_t a = 0, b = 0;
    st = update(c, in, inLength, out, outAvailable, &a);
    if (st == kSuccess) st = final(c, out ? out + a : nullptr, outAvailable - a, &b);
    if (moved) *moved = st == kSuccess ? a + b : (st == kBufferTooSmall ? needed : 0);
    release(c);
    return st;
}

} // namespace Hydrogen::Runtime::HostCrypto
