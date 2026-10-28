# HydrogenVM report — 6462f1e

2026-10-28T17:09:45+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 233.9s |
| cpu | 15 | 0 | 0 | 0.8s |
| ios | 6 | 0 | 0 | 2.1s |
| bench | 5 | 0 | 0 | 24.6s |

## Speed

CoreMark **1709.3** iterations/s (574.0 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **118.7%** | 712 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **89.0%** | 712 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **71.2%** | 570 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.6%** | 488 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 457 MIPS, ctests_arm 502 MIPS, ftests_vfp_thumb_softfp 439 MIPS, ftests_neon_thumb 404 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.67s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.04s | 61 | 0.46 ms | 7.86 ms | 0 |

## Problems (31)

### Below speed goal (1)

- **bench / coremark**: 37.6% of iPhone 5 single-core speed (goal: 100%)

### Warnings (30)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:143:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
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
- **build / compiler**: src/runtime/gl/HostGL.cpp:27:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:28:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:38:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:38:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:38:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:39:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:39:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/Posix.cpp:59:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/String.cpp:352:25: warning: null character(s) preserved in char literal [-Wnull-character]
- **build / compiler**: src/runtime/objc/Runtime.cpp:10:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

