// HydrogenVM — CommonCrypto ciphers forwarded to the host platform's crypto library.
//
// The emulator implements no cipher itself. On Apple hosts the calls go to the
// system's own CommonCrypto (the library the guest app was written against).
// On Linux, when OpenSSL is available at build time (HVM_HAVE_OPENSSL), they go
// to OpenSSL's EVP interface. Without either, every call reports
// kCCUnimplemented, as before.
#pragma once
#include <cstddef>
#include <cstdint>

namespace Hydrogen::Runtime::HostCrypto {

// CommonCrypto constants (CommonCryptor.h).
enum : int32_t {
    kSuccess = 0,
    kParamError = -4300,
    kBufferTooSmall = -4301,
    kMemoryFailure = -4302,
    kAlignmentError = -4303,
    kDecodeError = -4304,
    kUnimplemented = -4305,
    kKeySizeError = -4310,
};
enum : uint32_t { kEncrypt = 0, kDecrypt = 1 };
enum : uint32_t { kAES = 0, kDES = 1, k3DES = 2, kCAST = 3, kRC4 = 4, kRC2 = 5, kBlowfish = 6 };
enum : uint32_t { kOptionPKCS7Padding = 1, kOptionECBMode = 2 };

struct Cryptor;

// True when this build forwards to a real host library.
bool available();

int32_t create(uint32_t op, uint32_t alg, uint32_t options, const uint8_t* key, size_t keyLength,
               const uint8_t* iv, Cryptor** out);
int32_t update(Cryptor* c, const uint8_t* in, size_t inLength, uint8_t* out, size_t outAvailable, size_t* moved);
int32_t final(Cryptor* c, uint8_t* out, size_t outAvailable, size_t* moved);
int32_t reset(Cryptor* c, const uint8_t* iv);
size_t outputLength(Cryptor* c, size_t inLength, bool final);
void release(Cryptor* c);

// CCCrypt: create, update, final and release in one call.
int32_t oneShot(uint32_t op, uint32_t alg, uint32_t options, const uint8_t* key, size_t keyLength, const uint8_t* iv,
                const uint8_t* in, size_t inLength, uint8_t* out, size_t outAvailable, size_t* moved);

} // namespace Hydrogen::Runtime::HostCrypto
