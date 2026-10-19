# HydrogenVM report — 2999d9b

2026-10-19T21:25:59+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 266.4s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 6 | 0 | 0 | 2.0s |
| bench | 5 | 0 | 0 | 24.5s |

## Speed

CoreMark **1709.7** iterations/s (574.7 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **118.7%** | 712 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **89.0%** | 712 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **71.2%** | 570 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.6%** | 488 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 449 MIPS, ctests_arm 482 MIPS, ftests_vfp_thumb_softfp 391 MIPS, ftests_neon_thumb 341 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.78s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 61 | 0.54 ms | 10.34 ms | 0 |

## Problems (7)

### Below speed goal (1)

- **bench / coremark**: 37.6% of iPhone 5 single-core speed (goal: 100%)

### Warnings (6)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

