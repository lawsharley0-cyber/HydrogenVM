# HydrogenVM report — 55e2847

2026-10-23T20:53:18+00:00 · AMD EPYC 9V45 96-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 116.8s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 6 | 0 | 0 | 1.9s |
| bench | 5 | 0 | 0 | 16.7s |

## Speed

CoreMark **3102.0** iterations/s (1047.3 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **215.4%** | 1292 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **161.6%** | 1292 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **129.2%** | 1034 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **68.2%** | 886 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 822 MIPS, ctests_arm 895 MIPS, ftests_vfp_thumb_softfp 723 MIPS, ftests_neon_thumb 663 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.63s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.24 ms | 5.77 ms | 0 |

## Problems (29)

### Below speed goal (1)

- **bench / coremark**: 68.2% of iPhone 5 single-core speed (goal: 100%)

### Warnings (28)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
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
- **build / compiler**: src/runtime/gl/HostGL.cpp:24:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:25:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:34:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:34:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

