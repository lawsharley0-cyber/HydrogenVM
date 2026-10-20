# HydrogenVM report — c6f06e2

2026-10-20T18:48:06+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 136.5s |
| cpu | 15 | 0 | 0 | 0.4s |
| ios | 6 | 0 | 0 | 2.4s |
| bench | 5 | 0 | 0 | 25.1s |

## Speed

CoreMark **1591.1** iterations/s (559.5 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **110.5%** | 663 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **82.9%** | 663 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **66.3%** | 530 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **35.0%** | 455 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 492 MIPS, ctests_arm 534 MIPS, ftests_vfp_thumb_softfp 500 MIPS, ftests_neon_thumb 404 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 49 | 0.47 ms | 5.27 ms | 0 |

## Problems (11)

### Below speed goal (1)

- **bench / coremark**: 35.0% of iPhone 5 single-core speed (goal: 100%)

### Warnings (10)

- **build / compiler**: src/runtime/foundation/Extras.cpp:20:4: warning: unused function 'retainObj' [-Wunused-function]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

