# HydrogenVM report — e1825f5

2026-11-12T17:33:37+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 285.4s |
| tools | 1 | 0 | 0 | 25.4s |
| cpu | 15 | 0 | 0 | 0.7s |
| ios | 6 | 0 | 0 | 3.6s |
| bench | 5 | 0 | 0 | 24.9s |

## Speed

CoreMark **1253.3** iterations/s (434.4 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **87.0%** | 522 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **65.3%** | 522 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **52.2%** | 418 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **27.5%** | 358 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 291 MIPS, ctests_arm 318 MIPS, ftests_vfp_thumb_softfp 229 MIPS, ftests_neon_thumb 179 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.13s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.12s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.19s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.15s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.53s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.11s | 31 | 1.21 ms | 13.48 ms | 0 |

## Problems (42)

### Below speed goal (1)

- **bench / coremark**: 27.5% of iPhone 5 single-core speed (goal: 100%)

### Warnings (41)

- **build / compiler**: src/runtime/foundation/CompatibilityClasses.cpp:229:42: warning: missing field 'retained' initializer [-Wmissing-field-initializers]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:143:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/foundation/RunLoop.cpp:628:13: warning: unused variable 'cx' [-Wunused-variable]
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
- **build / compiler**: src/runtime/gl/HostGL.cpp:30:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:41:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:41:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:41:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:512:16: warning: unused function 'hgGetIntFwd' [-Wunused-function]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:102:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:105:46: warning: unused variable 'fFixed' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:20: warning: unused variable 'fScientific' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:39: warning: unused variable 'fShowbase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:56: warning: unused variable 'fShowpoint' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:75: warning: unused variable 'fShowpos' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:107:20: warning: unused variable 'fUppercase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:108:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:563:9: warning: unused function 'wsbEof' [-Wunused-function]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:564:9: warning: unused function 'wsbEofChar' [-Wunused-function]
- **build / compiler**: src/runtime/libc/Posix.cpp:59:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:10:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2006:6: warning: unused function 'avs_yesErr' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2028:10: warning: unused function 'avs_zero' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

