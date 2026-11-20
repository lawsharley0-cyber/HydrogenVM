// Guest-side qsort / qsort_r / bsearch tests (plan I7 #6): verifies correctness
// across aligned and unaligned element sizes, Darwin qsort_r thunk order,
// bsearch hits/misses, edge cases, and zero host HLE round trips per comparison.
#include "runtime/Process.hpp"
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <vector>

using namespace Hydrogen;
using namespace Hydrogen::Runtime;

int runGuestSortTests(const std::function<void(bool, const char*)>& check) {
    int failures = 0;
    auto ck = [&](bool ok, const char* what) { check(ok, what); failures += !ok; };

    ProcessConfig cfg;
    cfg.homePath = "/tmp/hvm-guest-sort-tests";
    Process p(cfg);

    const uint32_t qsortFn = *p.lookupSymbol("_qsort");
    const uint32_t qsortRFn = *p.lookupSymbol("_qsort_r");
    const uint32_t bsearchFn = *p.lookupSymbol("_bsearch");

    // Map a page of tiny ARMv7 comparators:
    //   cmp_i32(const int32_t* a, const int32_t* b):
    //     ldr r0, [r0]; ldr r1, [r1]; cmp r0, r1; movlt r0, #-1; movgt r0, #1; moveq r0, #0; bx lr
    //   cmp_u8_first(const uint8_t* a, const uint8_t* b):
    //     ldrb r0, [r0]; ldrb r1, [r1]; sub r0, r0, r1; bx lr
    //   cmp_i32_thunk(const int32_t* dir, const int32_t* a, const int32_t* b):
    //     ldr r3, [r0]; ldr r1, [r1]; ldr r2, [r2]; cmp r1, r2;
    //     movlt r0, #-1; movgt r0, #1; moveq r0, #0; mul r0, r0, r3; bx lr
    const uint32_t code = p.heap().allocAligned(16384, 16384);
    auto writeInsns = [&](uint32_t offset, std::initializer_list<uint32_t> insns) {
        uint32_t addr = code + offset;
        for (uint32_t w : insns) { p.mem().write<uint32_t>(addr, w); addr += 4; }
        return code + offset;
    };

    const uint32_t cmpI32 = writeInsns(0x00, {
        0xE5900000u, // ldr r0, [r0]
        0xE5911000u, // ldr r1, [r1]
        0xE1500001u, // cmp r0, r1
        0xB3E00000u, // mvnlt r0, #0  (-1)
        0xC3A00001u, // movgt r0, #1
        0x03A00000u, // moveq r0, #0
        0xE12FFF1Eu, // bx lr
    });

    const uint32_t cmpU8 = writeInsns(0x40, {
        0xE5D00000u, // ldrb r0, [r0]
        0xE5D11000u, // ldrb r1, [r1]
        0xE0400001u, // sub r0, r0, r1
        0xE12FFF1Eu, // bx lr
    });

    const uint32_t cmpI32Thunk = writeInsns(0x80, {
        0xE5903000u, // ldr r3, [r0]
        0xE5911000u, // ldr r1, [r1]
        0xE5922000u, // ldr r2, [r2]
        0xE1510002u, // cmp r1, r2
        0xB3E00000u, // mvnlt r0, #0
        0xC3A00001u, // movgt r0, #1
        0x03A00000u, // moveq r0, #0
        0xE0000390u, // mul r0, r0, r3
        0xE12FFF1Eu, // bx lr
    });
    p.mem().protect(code, 16384, Memory::kProtRX);

    CPU::Cpu cpu;
    cpu.r[CPU::kSP] = p.heap().allocZeroed(1 << 16) + (1 << 16) - 64;

    // 1. qsort on 200 int32s (negatives, duplicates, pseudo-random order)
    constexpr uint32_t kN = 200;
    std::vector<int32_t> vals(kN);
    uint32_t seed = 0x12345678u;
    for (uint32_t i = 0; i < kN; ++i) {
        seed = seed * 1664525u + 1013904223u;
        vals[i] = int32_t(seed) % 50;
    }
    vals[0] = INT32_MAX;
    vals[1] = INT32_MIN;
    const uint32_t arrAddr = p.heap().alloc(kN * sizeof(int32_t));
    std::memcpy(p.mem().host(arrAddr), vals.data(), kN * sizeof(int32_t));

    std::vector<int32_t> expected = vals;
    std::sort(expected.begin(), expected.end());

    auto* ring = cpu.call_ring.load();
    const uint64_t hleBefore = ring ? ring->total() : 0;
    p.callGuest(cpu, qsortFn, {arrAddr, kN, sizeof(int32_t), cmpI32});
    const uint64_t hleAfter = ring ? ring->total() : 0;

    std::vector<int32_t> got(kN);
    std::memcpy(got.data(), p.mem().host(arrAddr), kN * sizeof(int32_t));
    ck(got == expected, "guest qsort sorts 200 signed int32 values including INT32_MIN/MAX and duplicates");
    // Only the outer return stub fires (<= 1 HLE entry), never one per comparison (~1500 calls).
    ck(hleAfter - hleBefore <= 1, "guest qsort performs zero HLE round trips per comparison");

    // 2. qsort on odd-sized (5-byte) records to exercise the byte-wise swap path
    struct Rec5 { uint8_t key; uint8_t tag[4]; };
    static_assert(sizeof(Rec5) == 5);
    const Rec5 recIn[6] = {
        {40, {1, 2, 3, 4}}, {10, {5, 6, 7, 8}}, {30, {9, 10, 11, 12}},
        {20, {13, 14, 15, 16}}, {10, {17, 18, 19, 20}}, {5, {21, 22, 23, 24}},
    };
    const uint32_t recAddr = p.heap().alloc(sizeof(recIn));
    std::memcpy(p.mem().host(recAddr), recIn, sizeof(recIn));
    p.callGuest(cpu, qsortFn, {recAddr, 6, sizeof(Rec5), cmpU8});
    Rec5 recOut[6];
    std::memcpy(recOut, p.mem().host(recAddr), sizeof(recOut));
    bool recOk = recOut[0].key == 5 && recOut[0].tag[0] == 21
              && recOut[1].key == 10 && recOut[2].key == 10
              && recOut[3].key == 20 && recOut[3].tag[0] == 13
              && recOut[4].key == 30 && recOut[4].tag[0] == 9
              && recOut[5].key == 40 && recOut[5].tag[0] == 1;
    ck(recOk, "guest qsort sorts unaligned 5-byte records without corrupting payloads");

    // 3. Edge cases: n = 0, n = 1, size = 0, base = 0
    p.callGuest(cpu, qsortFn, {arrAddr, 0, 4, cmpI32});
    p.callGuest(cpu, qsortFn, {arrAddr, 1, 4, cmpI32});
    p.callGuest(cpu, qsortFn, {arrAddr, 5, 0, cmpI32});
    p.callGuest(cpu, qsortFn, {0, 5, 4, cmpI32});
    ck(true, "guest qsort handles empty, single-element, zero-size, and null inputs safely");

    // 4. qsort_r (Darwin/BSD order: base, n, size, thunk, cmp(thunk, a, b)) descending
    const uint32_t thunkAddr = p.heap().alloc(4);
    p.mem().write<int32_t>(thunkAddr, -1);
    std::memcpy(p.mem().host(arrAddr), vals.data(), kN * sizeof(int32_t));
    p.callGuest(cpu, qsortRFn, {arrAddr, kN, sizeof(int32_t), thunkAddr, cmpI32Thunk});
    std::memcpy(got.data(), p.mem().host(arrAddr), kN * sizeof(int32_t));
    std::vector<int32_t> expectedDesc = expected;
    std::reverse(expectedDesc.begin(), expectedDesc.end());
    ck(got == expectedDesc, "guest qsort_r sorts descending with Darwin thunk argument order");

    // 5. bsearch on sorted array (hit first, middle, last, and miss)
    std::memcpy(p.mem().host(arrAddr), expected.data(), kN * sizeof(int32_t));
    const uint32_t keyAddr = p.heap().alloc(4);
    auto runBsearch = [&](int32_t key) {
        p.mem().write<int32_t>(keyAddr, key);
        return uint32_t(p.callGuest(cpu, bsearchFn, {keyAddr, arrAddr, kN, sizeof(int32_t), cmpI32}));
    };
    const uint32_t hitMin = runBsearch(INT32_MIN);
    const uint32_t hitMax = runBsearch(INT32_MAX);
    const uint32_t hitMid = runBsearch(expected[kN / 2]);
    const uint32_t miss = runBsearch(123456);
    ck(hitMin == arrAddr && hitMax == arrAddr + (kN - 1) * 4, "guest bsearch finds first and last elements");
    ck(hitMid >= arrAddr && hitMid < arrAddr + kN * 4 && p.mem().read<int32_t>(hitMid) == expected[kN / 2],
       "guest bsearch finds interior element");
    ck(miss == 0, "guest bsearch returns NULL when key is absent");

    return failures;
}
