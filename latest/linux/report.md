# HydrogenVM report — 13f1013

2026-10-23T08:31:31+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 207.8s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 6 | 0 | 0 | 1.9s |
| bench | 5 | 0 | 0 | 24.5s |

## Speed

CoreMark **1715.8** iterations/s (573.7 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **119.1%** | 715 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **89.4%** | 715 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **71.5%** | 572 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.7%** | 490 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 478 MIPS, ctests_arm 516 MIPS, ftests_vfp_thumb_softfp 426 MIPS, ftests_neon_thumb 364 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.64s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.44 ms | 7.87 ms | 0 |

## Problems (17)

### Below speed goal (1)

- **bench / coremark**: 37.7% of iPhone 5 single-core speed (goal: 100%)

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

