# HydrogenVM report — 69e677d

2026-10-23T20:27:19+00:00 · AMD EPYC 9V45 96-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 113.5s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 5 | 1 | 0 | 1.2s |
| bench | 5 | 0 | 0 | 22.6s |

## Speed

CoreMark **3157.9** iterations/s (1068.8 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **219.3%** | 1316 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **164.5%** | 1316 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **131.6%** | 1053 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **69.4%** | 902 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 860 MIPS, ctests_arm 945 MIPS, ftests_vfp_thumb_softfp 939 MIPS, ftests_neon_thumb 862 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.01s | 61 | 0.21 ms | 4.03 ms | 0 |

## Problems (30)

### Failures (1)

- **ios / FoundationTest**

  ```
  build failed:
                        ^~~~~~~~~~~~~~~~~
  main.m:170:18: error: no visible @interface for 'NSMutableArray' declares the selector 'removeObjectsAtIndexes:'
    170 |         [maIndex removeObjectsAtIndexes:[NSIndexSet indexSetWithIndex:10]];
        |          ~~~~~~~ ^~~~~~~~~~~~~~~~~~~~~~
  main.m:177:12: error: no visible @interface for 'NSMutableArray' declares the selector 'insertObjects:atIndexes:'
    177 |     [maIns insertObjects:@[@"X", @"Y"] atIndexes:is13];
        |      ~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:182:16: error: no visible @interface for 'NSMutableArray' declares the selector 'insertObjects:atIndexes:'
    182 |         [maIns insertObjects:@[@"X"] atIndexes:is13];
        |          ~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:190:60: error: no known class method for selector 'indexSetWithIndex:'
    190 |         [maIns insertObjects:@[@"X"] atIndexes:[NSIndexSet indexSetWithIndex:100]];
        |                                                            ^~~~~~~~~~~~~~~~~
  main.m:190:16: error: no visible @interface for 'NSMutableArray' declares the selector 'insertObjects:atIndexes:'
    190 |         [maIns insertObjects:@[@"X"] atIndexes:[NSIndexSet indexSetWithIndex:100]];
        |          ~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:197:13: error: no visible @interface for 'NSMutableArray' declares the selector 'replaceObjectsAtIndexes:withObjects:'
    197 |     [maRepl replaceObjectsAtIndexes:is13 withObjects:@[@"X", @"Y"]];
        |      ~~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:202:17: error: no visible @interface for 'NSMutableArray' declares the selector 'replaceObjectsAtIndexes:withObjects:'
    202 |         [maRepl replaceObjectsAtIndexes:is13 withObjects:@[@"X"]];
        |          ~~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:210:53: error: no known class method for selector 'indexSetWithIndex:'
    210 |         [maRepl replaceObjectsAtIndexes:[NSIndexSet indexSetWithIndex:10] withObjects:@[@"X"]];
        |                                                     ^~~~~~~~~~~~~~~~~
  main.m:210:17: error: no visible @interface for 'NSMutableArray' declares the selector 'replaceObjectsAtIndexes:withObjects:'
    210 |         [maRepl replaceObjectsAtIndexes:[NSIndexSet indexSetWithIndex:10] withObjects:@[@"X"]];
        |          ~~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:217:33: error: no visible @interface for 'NSArray' declares the selector 'indexesOfObjectsPassingTest:'
    217 |     NSIndexSet *passed = [words indexesOfObjectsPassingTest:^BOOL(NSString *obj, NSUInteger idx, BOOL *stop) {
        |                           ~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~
  main.m:223:34: error: no visible @interface for 'NSArray' declares the selector 'indexesOfObjectsPassingTest:'
    223 |     NSIndexSet *stopped = [words indexesOfObjectsPassingTest:^BOOL(NSString *obj, NSUInteger idx, BOOL *stop) {
        |                            ~~~~~ ^~~~~~~~~~~~~~~~~~~~~~~~~~~
  17 errors generated.
  ```

### Below speed goal (1)

- **bench / coremark**: 69.4% of iPhone 5 single-core speed (goal: 100%)

### Warnings (28)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
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
- **build / compiler**: src/runtime/gl/HostGL.cpp:24:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:25:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:34:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:34:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

