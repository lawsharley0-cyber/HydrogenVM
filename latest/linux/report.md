# HydrogenVM report — 0a44706

2026-10-20T16:31:10+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 330.8s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 6 | 0 | 0 | 1.9s |
| bench | 5 | 0 | 0 | 18.8s |

## Speed

CoreMark **1722.1** iterations/s (570.6 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **119.6%** | 718 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **89.7%** | 718 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **71.8%** | 574 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.8%** | 492 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 457 MIPS, ctests_arm 471 MIPS, ftests_vfp_thumb_softfp 388 MIPS, ftests_neon_thumb 332 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.78s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.62 ms | 11.16 ms | 0 |

## Problems (11)

### Below speed goal (1)

- **bench / coremark**: 37.8% of iPhone 5 single-core speed (goal: 100%)

### Warnings (10)

- **build / compiler**: src/runtime/foundation/Extras.cpp:19:4: warning: unused function 'retainObj' [-Wunused-function]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:36:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:41:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:47:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

