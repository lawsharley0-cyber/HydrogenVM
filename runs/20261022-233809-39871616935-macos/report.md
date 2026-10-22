# HydrogenVM report — bd63291

2026-10-22T23:29:05+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 79.9s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 6 | 0 | 0 | 2.3s |
| bench | 5 | 0 | 0 | 20.1s |

## Speed

CoreMark **2574.5** iterations/s (868.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **178.8%** | 1073 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **134.1%** | 1073 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **107.3%** | 858 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **56.6%** | 736 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 535 MIPS, ctests_arm 571 MIPS, ftests_vfp_thumb_softfp 524 MIPS, ftests_neon_thumb 496 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 44 | 0.50 ms | 5.10 ms | 0 |

## Problems (10)

### Below speed goal (1)

- **bench / coremark**: 56.6% of iPhone 5 single-core speed (goal: 100%)

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

