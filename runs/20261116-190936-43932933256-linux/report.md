# HydrogenVM report — 0b9a169

2026-11-16T19:03:54+00:00 · Intel(R) Xeon(R) 6975P-C (x86_64, Linux 7.0.0-1014-aws)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 217.9s |
| tools | 1 | 0 | 0 | 5.3s |
| cpu | 15 | 0 | 0 | 0.4s |
| ios | 6 | 0 | 0 | 2.1s |
| bench | 5 | 0 | 0 | 18.8s |

## Speed

CoreMark **2776.4** iterations/s (929.4 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **192.8%** | 1157 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **144.6%** | 1157 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **115.7%** | 925 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **61.0%** | 793 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 618 MIPS, ctests_arm 666 MIPS, ftests_vfp_thumb_softfp 734 MIPS, ftests_neon_thumb 654 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.66s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.03s | 61 | 0.22 ms | 4.00 ms | 0 |

## Problems (50)

### Below speed goal (1)

- **bench / coremark**: 61.0% of iPhone 5 single-core speed (goal: 100%)

### Warnings (49)

- **build / compiler**: src/loader/MachO.cpp:213:31: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/foundation/CompatibilityClasses.cpp:229:42: warning: missing field 'retained' initializer [-Wmissing-field-initializers]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:143:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/foundation/RunLoop.cpp:651:13: warning: unused variable 'cx' [-Wunused-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:251:6: warning: unused function 'have' [-Wunused-function]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:37: warning: unused variable 'kUnsignedByte' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:61: warning: unused variable 'kByte' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:38:77: warning: unused variable 'kShort' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:51:73: warning: unused variable 'kTextureBinding2D' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:68:20: warning: unused variable 'kActiveTexture' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:77:73: warning: unused variable 'kElementArrayBufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:79:108: warning: unused variable 'kCullFace' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:79:87: warning: unused variable 'kDepthTest' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:80:20: warning: unused variable 'kCurrentProgram' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:81:20: warning: unused variable 'kFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/Gles1Emu.cpp:81:43: warning: unused variable 'kRenderbuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:31:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:42:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:43:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:43:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:513:16: warning: unused function 'hgGetIntFwd' [-Wunused-function]
- **build / compiler**: src/runtime/graphics/Text.cpp:69:133: warning: '&&' within '||' [-Wlogical-op-parentheses]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:102:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:105:46: warning: unused variable 'fFixed' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:20: warning: unused variable 'fScientific' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:39: warning: unused variable 'fShowbase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:56: warning: unused variable 'fShowpoint' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:106:75: warning: unused variable 'fShowpos' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:107:20: warning: unused variable 'fUppercase' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:108:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:594:9: warning: unused function 'wsbEof' [-Wunused-function]
- **build / compiler**: src/runtime/libc/LibcxxStreams.cpp:595:9: warning: unused function 'wsbEofChar' [-Wunused-function]
- **build / compiler**: src/runtime/libc/Net.cpp:561:10: warning: unused function 'n_nullHost' [-Wunused-function]
- **build / compiler**: src/runtime/libc/Posix.cpp:62:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:101:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/objc/Runtime.cpp:10:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:975:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:31:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:39:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2012:9: warning: unused function 'aug_ok3' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2261:6: warning: unused function 'avs_yesErr' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/PlatformStubs.cpp:2283:10: warning: unused function 'avs_zero' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIView.cpp:14:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: tests/runner/TestRunner.cpp:12555:29: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]

