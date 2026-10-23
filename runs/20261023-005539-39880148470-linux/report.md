# HydrogenVM report — 7f2d03c

2026-10-23T00:52:35+00:00 · Intel(R) Xeon(R) Platinum 8370C CPU @ 2.80GHz (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 153.6s |
| cpu | 15 | 0 | 0 | 0.2s |
| ios | 6 | 0 | 0 | 2.0s |
| bench | 5 | 0 | 0 | 20.6s |

## Speed

CoreMark **2040.7** iterations/s (683.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **141.7%** | 850 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **106.3%** | 850 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **85.0%** | 680 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **44.9%** | 583 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 478 MIPS, ctests_arm 516 MIPS, ftests_vfp_thumb_softfp 460 MIPS, ftests_neon_thumb 404 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.77s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.01s | 61 | 0.45 ms | 6.42 ms | 0 |

## Problems (10)

### Below speed goal (1)

- **bench / coremark**: 44.9% of iPhone 5 single-core speed (goal: 100%)

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

