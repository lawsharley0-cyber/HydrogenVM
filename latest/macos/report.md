# HydrogenVM report — 0489392

2026-10-23T07:54:59+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 126.2s |
| cpu | 15 | 0 | 0 | 0.4s |
| ios | 6 | 0 | 0 | 2.5s |
| bench | 5 | 0 | 0 | 23.2s |

## Speed

CoreMark **1833.6** iterations/s (606.4 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **127.3%** | 764 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **95.5%** | 764 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **76.4%** | 611 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **40.3%** | 524 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 481 MIPS, ctests_arm 543 MIPS, ftests_vfp_thumb_softfp 477 MIPS, ftests_neon_thumb 394 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.03s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.10s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.03s | 29 | 0.91 ms | 5.61 ms | 0 |

## Problems (17)

### Below speed goal (1)

- **bench / coremark**: 40.3% of iPhone 5 single-core speed (goal: 100%)

### Warnings (16)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:23:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:24:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

