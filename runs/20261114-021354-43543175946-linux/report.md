# HydrogenVM report — 70ac319

2026-11-14T02:07:23+00:00 · AMD EPYC 9V74 80-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 261.1s |
| tools | 1 | 0 | 0 | 9.3s |
| cpu | 15 | 0 | 0 | 0.6s |
| ios | 6 | 0 | 0 | 2.2s |
| bench | 5 | 0 | 0 | 18.6s |

## Speed

CoreMark **2275.7** iterations/s (760.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **158.0%** | 948 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **118.5%** | 948 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **94.8%** | 759 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **50.0%** | 650 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 578 MIPS, ctests_arm 619 MIPS, ftests_vfp_thumb_softfp 542 MIPS, ftests_neon_thumb 480 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.67s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.04s | 61 | 0.35 ms | 7.84 ms | 0 |

## Problems (49)

### Below speed goal (1)

- **bench / coremark**: 50.0% of iPhone 5 single-core speed (goal: 100%)

### Warnings (48)

- **build / compiler**: src/loader/MachO.cpp:213:31: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/foundation/CompatibilityClasses.cpp:229:42: warning: missing field 'retained' initializer [-Wmissing-field-initializers]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:143:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/foundation/RunLoop.cpp:641:13: warning: unused variable 'cx' [-Wunused-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:251:6: warning: unused function 'have' [-Wunused-function]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:37: warning: unused variable 'kUnsignedByte' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:61: warning: unused variable 'kByte' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:77: warning: unused variable 'kShort' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:51:73: warning: unused variable 'kTextureBinding2D' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:68:20: warning: unused variable 'kActiveTexture' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:77:73: warning: unused variable 'kElementArrayBufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:79:108: warning: unused variable 'kCullFace' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:79:87: warning: unused variable 'kDepthTest' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:80:20: warning: unused variable 'kCurrentProgram' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:81:20: warning: unused variable 'kFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:81:43: warning: unused variable 'kRenderbuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:31:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:43:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:43:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:513:16: warning: unused function 'hgGetIntFwd' [-Wunused-function]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:102:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:105:46: warning: unused variable 'fFixed' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:20: warning: unused variable 'fScientific' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:39: warning: unused variable 'fShowbase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:56: warning: unused variable 'fShowpoint' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:75: warning: unused variable 'fShowpos' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:107:20: warning: unused variable 'fUppercase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:108:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:594:9: warning: unused function 'wsbEof' [-Wunused-function]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:595:9: warning: unused function 'wsbEofChar' [-Wunused-function]
- **build / compiler**: src/runtime/libc/Net.cpp:560:10: warning: unused function 'n_nullHost' [-Wunused-function]
- **build / compiler**: src/runtime/libc/Posix.cpp:59:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:101:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/objc/Runtime.cpp:10:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:964:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:29:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:37:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:1993:9: warning: unused function 'aug_ok3' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2229:6: warning: unused function 'avs_yesErr' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2251:10: warning: unused function 'avs_zero' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIView.cpp:13:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: tests/runner/TestRunner.cpp:12249:29: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]

