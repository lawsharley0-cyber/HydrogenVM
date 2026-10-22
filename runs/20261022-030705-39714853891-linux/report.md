# HydrogenVM report — 70fb714

2026-10-22T03:04:53+00:00 · Intel(R) Xeon(R) 6973P-C (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 105.7s |
| cpu | 15 | 0 | 0 | 0.2s |
| ios | 6 | 0 | 0 | 2.0s |
| bench | 5 | 0 | 0 | 17.7s |

## Speed

CoreMark **2946.8** iterations/s (988.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **204.6%** | 1228 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **153.5%** | 1228 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **122.8%** | 982 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **64.8%** | 842 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 655 MIPS, ctests_arm 693 MIPS, ftests_vfp_thumb_softfp 647 MIPS, ftests_neon_thumb 632 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.77s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.01s | 61 | 0.26 ms | 5.01 ms | 0 |

## Problems (10)

### Below speed goal (1)

- **bench / coremark**: 64.8% of iPhone 5 single-core speed (goal: 100%)

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

