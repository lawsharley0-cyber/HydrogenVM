# HydrogenVM report — 2999d9b

2026-10-19T21:32:09+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 120.4s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 6 | 0 | 0 | 2.3s |
| bench | 5 | 0 | 0 | 20.6s |

## Speed

CoreMark **1541.8** iterations/s (520.5 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **107.1%** | 642 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **80.3%** | 642 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **64.2%** | 514 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **33.9%** | 441 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 422 MIPS, ctests_arm 422 MIPS, ftests_vfp_thumb_softfp 354 MIPS, ftests_neon_thumb 416 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.03s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 47 | 0.76 ms | 6.85 ms | 0 |

## Problems (7)

### Below speed goal (1)

- **bench / coremark**: 33.9% of iPhone 5 single-core speed (goal: 100%)

### Warnings (6)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

