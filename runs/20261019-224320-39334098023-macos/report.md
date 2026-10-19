# HydrogenVM report — a75ba3d

2026-10-19T22:26:01+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 104.1s |
| cpu | 15 | 0 | 0 | 0.4s |
| ios | 6 | 0 | 0 | 2.4s |
| bench | 5 | 0 | 0 | 23.8s |

## Speed

CoreMark **1741.8** iterations/s (592.6 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **121.0%** | 726 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **90.7%** | 726 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **72.6%** | 581 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **38.3%** | 498 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 475 MIPS, ctests_arm 504 MIPS, ftests_vfp_thumb_softfp 429 MIPS, ftests_neon_thumb 334 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.99s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.03s | 49 | 0.91 ms | 10.31 ms | 0 |

## Problems (8)

### Below speed goal (1)

- **bench / coremark**: 38.3% of iPhone 5 single-core speed (goal: 100%)

### Warnings (7)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:82:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

