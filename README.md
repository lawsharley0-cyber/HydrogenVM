# HydrogenVM

A JIT-less execution engine for 32-bit ARM (ARMv7-A, ARM + Thumb-2) code on
modern 64-bit hosts. The goal is running legacy 32-bit iOS apps on current
Apple devices without JIT entitlements; Android `armeabi-v7a` native code is a
later phase.

**Status:**
- **CPU Core:** Complete (ARMv7 integer, VFPv3/v4, NEON; ARM + Thumb-2) with lazy block decoding, dead-flag elimination, branch/loop fusion (`CMP`/`CMN`/`TST`/`SUBS`/`ADDS` + `B<cond>`), and intra-block superinstruction fusion (`MOVW`+`MOVT`, `BL`/`BLX`, `POP {..., pc}`, `LDR`+`LDR`, `STR`+`STR`, `LDR`+`ADD`/`SUB`, `STR`+`ADD`/`SUB`, `MOV`+`ADD`, `MOV`+`MOV`, `ADD`+`ADD`).
- **Loaders & Runtime:** Mach-O loader, guest Objective-C runtime, `libSystem`, `Foundation` / `CoreFoundation` / `GCD`, `CoreGraphics`, and `QuartzCore` 60 Hz layer compositor.
- **UIKit:** Views, controls, gestures, alerts/sheets, NIB loader (`NIBArchive` and keyed archives), `UIStoryboard` / `UIStoryboardSegue`, `UIScrollView`, `UITableView` / `UITableViewCell` / `UITableViewController`, `UINavigationController` / `UINavigationBar` / `UINavigationItem` / `UIBarButtonItem` / `UIToolbar`, `UITabBarController` / `UITabBar` / `UITabBarItem`, `UITextField` / `UITextView`, `UISwitch`, `UISlider`, `UISegmentedControl`, `UIProgressView`, and `UIPageControl`.
- **OpenGL ES 1.1 & 2.0:** `CAEAGLLayer`, `EAGLContext`, `EAGLSharegroup`, FBOs/RBOs, VBOs/VAOs, 2D textures, GLES 1.1 fixed-function pipeline & matrix stacks (`GL_OES_draw_texture`, `GL_OES_framebuffer_object`), and GLES 2.0 shaders/programs/uniforms/attributes rasterizing into `CAEAGLLayer`.
- **Audio:** `AVAudioPlayer`, `AVAudioSession`, `AudioToolbox` (`AudioQueue`, `AudioServices`, `AudioFile`, `AudioUnit`), and `OpenAL` (`alc*` / `al*`) with WAV/PCM decoding and stereo PCM streaming bridged to `AVAudioEngine` in the iOS host app.
- **iPhone App (`app/`, bundle ID `com.lfull.HydrogenVM`, iOS 26+):** Imports `.ipa` files and runs them with a live screen, multi-touch, accelerometer, `AVAudioEngine` sound output, and logs. See [`docs/ARCHITECTURE.md`](docs/ARCHITECTURE.md) for the design and architecture.

## Build & test

Builds with Clang (using `[[clang::musttail]]`) or GCC (using `-foptimize-sibling-calls`), CMake, and Ninja.

```sh
tools/run_tests.sh
```

This builds the library, the test runner and benchmark, compiles the guest
test programs, and runs them all. Expected output ends with every line showing
`0 failed`.

```sh
build/hvm_tests selftest    # built-in ARM + Thumb CPU smoke test & MIPS benchmark
build/hvm_tests fulltest    # end-to-end CPU superinstructions, UIKit, OpenGL ES 1.1/2.0, and Audio suite
tools/ios_tests.sh          # 32-bit iOS test apps through the full runtime (needs ld64)
build/hvm run path/to/Foo.app --run-for 3 --screenshot shot.png --tap 160,240@1
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
