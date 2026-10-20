# HydrogenVM report — 06949d9

2026-10-20T00:01:55+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 69.5s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 6 | 0 | 0 | 2.4s |
| bench | 5 | 0 | 0 | 20.6s |

## Speed

CoreMark **2527.6** iterations/s (849.2 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **175.5%** | 1053 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **131.6%** | 1053 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **105.3%** | 843 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **55.6%** | 722 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 508 MIPS, ctests_arm 542 MIPS, ftests_vfp_thumb_softfp 523 MIPS, ftests_neon_thumb 521 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.02s | 46 | 0.61 ms | 5.24 ms | 0 |

## Problems (8)

### Below speed goal (1)

- **bench / coremark**: 55.6% of iPhone 5 single-core speed (goal: 100%)

### Warnings (7)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:82:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

