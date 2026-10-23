# HydrogenVM report — ff427f8

2026-10-23T20:47:24+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 147.7s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 6 | 0 | 0 | 2.4s |
| bench | 5 | 0 | 0 | 26.8s |

## Speed

CoreMark **1485.7** iterations/s (524.1 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **103.2%** | 619 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **77.4%** | 619 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **61.9%** | 495 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **32.7%** | 424 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 416 MIPS, ctests_arm 466 MIPS, ftests_vfp_thumb_softfp 487 MIPS, ftests_neon_thumb 436 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.99s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.04s | 50 | 0.94 ms | 10.24 ms | 0 |

## Problems (29)

### Below speed goal (1)

- **bench / coremark**: 32.7% of iPhone 5 single-core speed (goal: 100%)

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

