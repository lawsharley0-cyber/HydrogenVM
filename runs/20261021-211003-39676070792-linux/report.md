# HydrogenVM report — 71b5d06

2026-10-21T21:07:05+00:00 · INTEL(R) XEON(R) PLATINUM 8573C (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 148.1s |
| cpu | 15 | 0 | 0 | 0.2s |
| ios | 6 | 0 | 0 | 1.9s |
| bench | 5 | 0 | 0 | 19.6s |

## Speed

CoreMark **2151.0** iterations/s (720.2 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **149.4%** | 896 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **112.0%** | 896 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **89.6%** | 717 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **47.3%** | 615 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 475 MIPS, ctests_arm 505 MIPS, ftests_vfp_thumb_softfp 534 MIPS, ftests_neon_thumb 448 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.03s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.77s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.43 ms | 7.67 ms | 0 |

## Problems (10)

### Below speed goal (1)

- **bench / coremark**: 47.3% of iPhone 5 single-core speed (goal: 100%)

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

