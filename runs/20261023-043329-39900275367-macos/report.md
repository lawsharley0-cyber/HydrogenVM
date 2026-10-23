# HydrogenVM report — 363bbef

2026-10-23T04:30:21+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 156.8s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 5 | 1 | 0 | 2.7s |
| bench | 5 | 0 | 0 | 20.3s |

## Speed

CoreMark **2066.3** iterations/s (701.3 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **143.5%** | 861 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **107.6%** | 861 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **86.1%** | 689 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **45.4%** | 590 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 406 MIPS, ctests_arm 365 MIPS, ftests_vfp_thumb_softfp 369 MIPS, ftests_neon_thumb 346 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.04s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.03s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 1.20s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.04s | 28 | 1.91 ms | 12.00 ms | 0 |

## Problems (11)

### Failures (1)

- **ios / FoundationTest**

  ```
  exit 1 (want 0); expected line missing
  FAIL NSTimer repeats then invalidates (line 306)
  --- stderr ---
  NSLog works: yes 1
  ```

### Below speed goal (1)

- **bench / coremark**: 45.4% of iPhone 5 single-core speed (goal: 100%)

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

