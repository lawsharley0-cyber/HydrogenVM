// Shared allocator regressions, separate from the actively edited GLES suite.
#include "runtime/Process.hpp"
#include <cstdio>
#include <filesystem>
#include <unistd.h>

int main() {
    using namespace Hydrogen;
    using namespace Hydrogen::Runtime;
    int failed = 0, passed = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s %s\n", ok ? "ok" : "FAIL", name);
        ok ? ++passed : ++failed;
    };
    {
        Memory::GuestArena mem;
        Heap heap(mem, 0x10000000, 0x11000000);
        const auto before = heap.stats();
        bool rejected = true;
        for (uint32_t size : {0x7ffffff1u, 0xfffff000u, 0xfffffff0u, 0xffffffffu}) {
            for (uint32_t alignment : {4u, 16u, 32u, 4096u, 0x80000000u}) {
                const uint32_t p = heap.allocAligned(size, alignment);
                rejected &= p == 0;
                if (p) heap.free(p);
            }
        }
        const auto after = heap.stats();
        check(rejected && before.liveBytes == after.liveBytes && before.liveBlocks == after.liveBlocks &&
              before.committed == after.committed, "oversized aligned requests fail without heap mutation");
        check(heap.allocAligned(64, 0) == 0 && heap.allocAligned(64, 24) == 0 &&
              heap.allocAligned(64, 3) == 0, "heap rejects zero and non-power-of-two alignment");
        // Keep positive cases independent of allocator corruption on an old build.
        Heap validHeap(mem, 0x12000000, 0x13000000);
        bool valid = true;
        for (uint32_t alignment : {4u, 16u, 32u, 4096u, 65536u}) {
            for (uint32_t size : {0u, 1u, 2048u, 2049u, 8192u}) {
                const auto p = validHeap.allocAligned(size, alignment);
                valid &= p && !(p & (alignment - 1)) && validHeap.blockSize(p) >= size;
                if (p && size) { mem.write<uint8_t>(p, 0x5a); mem.write<uint8_t>(p + size - 1, 0xa5); }
                validHeap.free(p);
            }
        }
        check(valid && validHeap.stats().liveBlocks == 0 && validHeap.stats().badFrees == 0,
              "valid aligned allocations have writable capacity and can be freed");
    }

    {
        Memory::GuestArena mem;
        Heap heap(mem, 0x10000000, 0x11000000);
        const auto before = heap.stats();

        for (int i = 0; i < 100; ++i) {
            uint32_t a = heap.alloc(100);
            uint32_t b = heap.allocZeroed(200);
            uint32_t c = heap.allocAligned(300, 4096);

            uint32_t a2 = heap.realloc(a, 150);
            uint32_t b2 = heap.realloc(b, 250);

            heap.free(a2);
            heap.free(b2);
            heap.free(c);
        }

        const auto after = heap.stats();
        check(before.liveBytes == after.liveBytes && before.liveBlocks == after.liveBlocks,
              "heap allocation/reallocation/deallocation symmetry leaves no leaked bytes");

        // POSIX: realloc failure leaves the original block allocated.
        uint32_t a = heap.alloc(100);
        const uint32_t oldSize = heap.blockSize(a);
        mem.write<uint8_t>(a, 0x5a);
        mem.write<uint8_t>(a + 99, 0xa5);
        const auto beforeFail = heap.stats();
        const uint32_t large = heap.realloc(a, 0x7FFFFFF0u);
        const auto afterFail = heap.stats();
        check(a != 0 && large == 0, "oversized realloc fails");
        check(oldSize >= 100 && heap.blockSize(a) == oldSize, "failed realloc preserves the original block size");
        check(mem.read<uint8_t>(a) == 0x5a && mem.read<uint8_t>(a + 99) == 0xa5,
              "failed realloc preserves the original bytes");
        check(beforeFail.liveBlocks == afterFail.liveBlocks && beforeFail.liveBytes == afterFail.liveBytes &&
              beforeFail.badFrees == afterFail.badFrees,
              "failed realloc does not change live heap stats");
        heap.free(a);
        check(heap.blockSize(a) == 0 && heap.stats().badFrees == beforeFail.badFrees,
              "the original block can still be freed after a failed realloc");
    }

    const auto home = std::filesystem::temp_directory_path() / ("hvm-heap-tests-" + std::to_string(getpid()));
    {
        ProcessConfig cfg;
        cfg.homePath = home.string();
        cfg.bundlePath = (home / "Test.app").string();
        Process p(cfg);
        auto& cpu = p.mainThread().cpu;
        p.mem().map(0xbee00000, 1u << 20, Memory::kProtRW);
        cpu.r[CPU::kSP] = 0xbef00000 - 256;
        const auto out = p.heap().allocZeroed(4);
        p.mainThread().errnoAddr = p.heap().allocZeroed(4);
        p.mem().write<uint32_t>(p.mainThread().errnoAddr, 77);
        auto call = [&](const char* name, std::initializer_list<uint32_t> args) {
            return uint32_t(p.callGuest(cpu, *p.lookupSymbol(name), args));
        };
        bool invalid = true;
        for (uint32_t alignment : {0u, 1u, 2u, 3u, 6u, 24u}) {
            p.mem().write<uint32_t>(out, 0x12345678);
            invalid &= call("_posix_memalign", {out, alignment, 64}) == 22 &&
                       p.mem().read<uint32_t>(out) == 0x12345678;
        }
        check(invalid && p.mem().read<uint32_t>(p.mainThread().errnoAddr) == 77,
              "posix_memalign returns guest EINVAL without changing output or errno");
        bool exhausted = call("_posix_memalign", {out, 4096, 0xfffffff0}) == 12 &&
                         p.mem().read<uint32_t>(out) == 0x12345678 &&
                         call("_valloc", {0xffffffff}) == 0 &&
                         call("_malloc_zone_memalign", {0, 4096, 0xfffffff0}) == 0;
        check(exhausted, "aligned libc entry points reject wrapped sizes and preserve failed output");
        const bool allocated = call("_posix_memalign", {out, 4, 31}) == 0;
        const auto block = p.mem().read<uint32_t>(out);
        check(allocated && block && !(block & 3) && p.heap().blockSize(block) >= 31,
              "posix_memalign uses the four-byte ARMv7 pointer alignment on a 64-bit host");
        if (allocated) call("_free", {block});
        p.heap().free(out);
        p.heap().free(p.mainThread().errnoAddr);
    }
    std::filesystem::remove_all(home);
    std::printf("heap tests: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
