# HydrogenVM report — a2abc9b

2026-11-01T05:41:40+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**All tests passed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 239.8s |
| tools | 1 | 0 | 0 | 6.7s |
| cpu | 15 | 0 | 0 | 0.7s |
| ios | 6 | 0 | 0 | 2.1s |
| bench | 5 | 0 | 0 | 25.0s |

## Speed

CoreMark **1683.7** iterations/s (565.3 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **116.9%** | 702 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **87.7%** | 702 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **70.2%** | 561 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.0%** | 481 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 446 MIPS, ctests_arm 485 MIPS, ftests_vfp_thumb_softfp 404 MIPS, ftests_neon_thumb 431 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.06s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.66s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.04s | 61 | 0.42 ms | 7.25 ms | 0 |

## Problems (34)

### Below speed goal (1)

- **bench / coremark**: 37.0% of iPhone 5 single-core speed (goal: 100%)

### Warnings (33)

- **build / compiler**: src/loader/MachO.cpp:206:31: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/foundation/NSNumber.cpp:143:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/foundation/RunLoop.cpp:514:13: warning: unused variable 'cx' [-Wunused-variable]
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
- **build / compiler**: src/runtime/gl/HostGL.cpp:30:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:40:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:40:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:40:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:41:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:41:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/Posix.cpp:59:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:10:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:59:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/objc/Runtime.cpp:922:10: warning: 'atomic_store<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]
- **build / compiler**: tests/runner/TestRunner.cpp:6746:29: warning: 'atomic_load<Hydrogen::Loader::MethodSymbols>' is deprecated: use 'std::atomic<std::shared_ptr<T>>' instead [-Wdeprecated-declarations]

