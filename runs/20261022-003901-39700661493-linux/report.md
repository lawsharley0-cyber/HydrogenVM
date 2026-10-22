# HydrogenVM report — 4cfd06e

2026-10-22T00:36:22+00:00 · INTEL(R) XEON(R) PLATINUM 8573C (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 131.9s |
| cpu | 15 | 0 | 0 | 0.2s |
| ios | 6 | 0 | 0 | 1.9s |
| bench | 5 | 0 | 0 | 17.5s |

## Speed

CoreMark **2406.2** iterations/s (806.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **167.1%** | 1003 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **125.3%** | 1003 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **100.3%** | 802 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **52.9%** | 687 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 473 MIPS, ctests_arm 510 MIPS, ftests_vfp_thumb_softfp 539 MIPS, ftests_neon_thumb 522 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.77s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.01s | 61 | 0.35 ms | 5.83 ms | 0 |

## Problems (10)

### Below speed goal (1)

- **bench / coremark**: 52.9% of iPhone 5 single-core speed (goal: 100%)

### Warnings (9)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

