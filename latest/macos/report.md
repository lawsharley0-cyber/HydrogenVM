# HydrogenVM report — 4dc9586

2026-10-20T06:01:49+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 135.4s |
| cpu | 15 | 0 | 0 | 0.6s |
| ios | 6 | 0 | 0 | 2.5s |
| bench | 5 | 0 | 0 | 20.4s |

## Speed

CoreMark **2141.5** iterations/s (693.1 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **148.7%** | 892 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **111.5%** | 892 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **89.2%** | 714 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **47.1%** | 612 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 406 MIPS, ctests_arm 428 MIPS, ftests_vfp_thumb_softfp 453 MIPS, ftests_neon_thumb 347 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.04s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.03s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.99s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.05s | 49 | 0.80 ms | 9.00 ms | 0 |

## Problems (11)

### Below speed goal (1)

- **bench / coremark**: 47.1% of iPhone 5 single-core speed (goal: 100%)

### Warnings (10)

- **build / compiler**: src/runtime/foundation/Extras.cpp:19:4: warning: unused function 'retainObj' [-Wunused-function]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:36:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:41:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:47:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

