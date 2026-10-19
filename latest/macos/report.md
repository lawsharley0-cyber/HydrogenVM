# HydrogenVM report — c1bef59

2026-10-19T22:51:12+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 105.6s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 6 | 0 | 0 | 2.4s |
| bench | 5 | 0 | 0 | 22.3s |

## Speed

CoreMark **1947.2** iterations/s (636.7 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **135.2%** | 811 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **101.4%** | 811 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **81.1%** | 649 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **42.8%** | 556 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 405 MIPS, ctests_arm 417 MIPS, ftests_vfp_thumb_softfp 308 MIPS, ftests_neon_thumb 260 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.00s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.07s | 46 | 0.93 ms | 10.12 ms | 0 |

## Problems (8)

### Below speed goal (1)

- **bench / coremark**: 42.8% of iPhone 5 single-core speed (goal: 100%)

### Warnings (7)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:82:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

