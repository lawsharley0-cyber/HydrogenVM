# HydrogenVM report — c1bef59

2026-10-19T22:45:28+00:00 · AMD EPYC 9V74 80-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 250.3s |
| cpu | 15 | 0 | 0 | 0.4s |
| ios | 6 | 0 | 0 | 2.0s |
| bench | 5 | 0 | 0 | 24.2s |

## Speed

CoreMark **1732.4** iterations/s (582.1 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **120.3%** | 722 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **90.2%** | 722 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **72.2%** | 577 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **38.1%** | 495 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 432 MIPS, ctests_arm 468 MIPS, ftests_vfp_thumb_softfp 358 MIPS, ftests_neon_thumb 333 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.79s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.53 ms | 9.76 ms | 0 |

## Problems (8)

### Below speed goal (1)

- **bench / coremark**: 38.1% of iPhone 5 single-core speed (goal: 100%)

### Warnings (7)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:82:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

