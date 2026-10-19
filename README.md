# HydrogenVM

A JIT-less execution engine for 32-bit ARM (ARMv7-A, ARM + Thumb-2) code on
modern 64-bit hosts. The goal is running legacy 32-bit iOS apps on current
Apple devices without JIT entitlements; Android `armeabi-v7a` native code is a
later phase.

**Status:** CPU core complete (ARMv7 integer, VFPv3/v4, NEON; ARM + Thumb-2).
Mach-O loader, guest Objective-C runtime, libSystem, Foundation/CoreFoundation/GCD,
CoreGraphics, a QuartzCore compositor and core UIKit are implemented and pass
end-to-end tests with real 32-bit iOS executables. The iPhone app (`app/`,
bundle ID `com.lfull.HydrogenVM`, iOS 26+) imports `.ipa` files and runs them
with a live screen, touch and logs. OpenGL ES → Metal and audio are next. See
[`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design, limitations and
roadmap.

## Build & test

Requires clang (the core uses `[[clang::musttail]]`), CMake, Ninja, and
`ld.lld` for building the guest test binaries.

```sh
tools/run_tests.sh
```

This builds the library, the test runner and benchmark, compiles the guest
test programs, and runs them all. Expected output ends with every line showing
`0 failed`.

```sh
tools/ios_tests.sh          # 32-bit iOS test apps through the full runtime (needs ld64)
build/hvm path/to/Foo.app --run-for 3 --screenshot shot.png --tap 160,240@1
```

```sh
build/hvm_bench build/tests/ctests_thumb.elf 30          # speed
python3 tools/decode_fuzz.py build/hvm_decodecheck 400000 # decoder coverage vs llvm-mc
```

### iPhone app

```sh
brew install xcodegen
cd app && xcodegen && open HydrogenVM.xcodeproj
```

Set your team in Signing & Capabilities, then build to a device (the 4 GB
guest address space needs real hardware and the extended virtual addressing
entitlement).

## Using the engine

```cpp
Hydrogen::Memory::GuestArena mem;           // 4 GB guest address space
Hydrogen::CPU::Engine engine(mem);

mem.map(0x10000, codeSize, Hydrogen::Memory::kProtRX);
mem.copyIn(0x10000, code, codeSize);

uint32_t puts_addr = engine.registerHle("puts", [&](Hydrogen::CPU::Cpu& c) {
    std::puts(reinterpret_cast<const char*>(mem.hostOrNull(c.r[0])));
});

Hydrogen::CPU::Cpu cpu;
cpu.r[13] = stackTop;
cpu.r[15] = entry & ~1u;
cpu.thumb = entry & 1;
Hydrogen::CPU::Exit why = engine.run(cpu, /*budget*/ 1'000'000);
```

`run` returns when the time-slice budget is used up (call it again to
resume), on SVC, on an undefined or unimplemented instruction, on a memory
fault, or when an HLE function sets `cpu.exit`.
