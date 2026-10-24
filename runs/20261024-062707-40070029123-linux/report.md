# HydrogenVM report — f6c02c6

2026-10-24T06:24:08+00:00 · INTEL(R) XEON(R) PLATINUM 8573C (x86_64, Linux 6.17.0-1022-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 149.2s |
| cpu | 15 | 0 | 0 | 0.5s |
| ios | 5 | 1 | 0 | 1.2s |
| bench | 5 | 0 | 0 | 21.1s |

## Speed

CoreMark **2468.1** iterations/s (830.5 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **171.4%** | 1028 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **128.5%** | 1028 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **102.8%** | 823 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **54.2%** | 705 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 560 MIPS, ctests_arm 582 MIPS, ftests_vfp_thumb_softfp 567 MIPS, ftests_neon_thumb 587 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 1.01s | 61 | 0.34 ms | 5.68 ms | 0 |

## Problems (33)

### Failures (1)

- **ios / FoundationTest**

  ```
  exit 70 (want 0); expected line missing
  ok fileExistsAtPath:isDirectory:
  ok contentsOfDirectoryAtPath
  ok attributesOfItemAtPath
  ok remove missing file error
  ok array plist file round trip
  ok NSURL scheme
  ok NSURL host
  ok NSURL port
  ok NSURL path
  ok NSURL query
  ok NSURL lastPathComponent
  ok fileURLWithPath
  ok file URL path
  ok URLForResource
  ok registerDefaults
  ok NSUserDefaults values
  ok NSUserDefaults synchronize
  ok defaults persisted to plist
  ok NSTemporaryDirectory
  ok CFStringCreateWithCString (bridged)
  ok CFStringAppend / GetCString
  ok CFStringFind
  ok CFStringCompare
  ok CFStringCreateWithFormat
  ok CFNumber
  --- stderr ---
  [hvm:error] Crash: bad memory access at 0xffff6208 (inside missing data symbol __kCFNumberNaN)
    pc [HydrogenVM] CFNumberGetValue
    lr 0x00010e51 FoundationTest+52816 _main + 0x7980
    r0  ffff6208  r1  0000000d  r2  befffc48  r3  00000070
    r4  ffffffff  r5  00000001  r6  ffffffff  r7  beffffb8
    r8  0001925c  r9  befff474  r10 00019154  r11 00019258
    r12 0001809c  r13 befff2c0  r14 00010e51  r15 ffff2d00
    backtrace:
      [HydrogenVM] CFNumberGetValue
      0x00010e51 FoundationTest+52816 _main + 0x7980
      [HydrogenVM] _hvm_return_to_host
  ```

### Crashes (1)

- **ios / FoundationTest**

  ```
  Crash: bad memory access at 0xffff6208 (inside missing data symbol __kCFNumberNaN)
    pc [HydrogenVM] CFNumberGetValue
    lr 0x00010e51 FoundationTest+52816 _main + 0x7980
    r0  ffff6208  r1  0000000d  r2  befffc48  r3  00000070
    r4  ffffffff  r5  00000001  r6  ffffffff  r7  beffffb8
    r8  0001925c  r9  befff474  r10 00019154  r11 00019258
    r12 0001809c  r13 befff2c0  r14 00010e51  r15 ffff2d00
    backtrace:
      [HydrogenVM] CFNumberGetValue
      0x00010e51 FoundationTest+52816 _main + 0x7980
      [HydrogenVM] _hvm_return_to_host
  ```

### Runtime errors (1)

- **ios / FoundationTest**

  ```
  Crash: bad memory access at 0xffff6208 (inside missing data symbol __kCFNumberNaN)
    pc [HydrogenVM] CFNumberGetValue
    lr 0x00010e51 FoundationTest+52816 _main + 0x7980
    r0  ffff6208  r1  0000000d  r2  befffc48  r3  00000070
    r4  ffffffff  r5  00000001  r6  ffffffff  r7  beffffb8
    r8  0001925c  r9  befff474  r10 00019154  r11 00019258
    r12 0001809c  r13 befff2c0  r14 00010e51  r15 ffff2d00
    backtrace:
      [HydrogenVM] CFNumberGetValue
      0x00010e51 FoundationTest+52816 _main + 0x7980
      [HydrogenVM] _hvm_return_to_host
  ```

### Below speed goal (1)

- **bench / coremark**: 54.2% of iPhone 5 single-core speed (goal: 100%)

### Warnings (29)

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
- **build / compiler**: src/runtime/gl/HostGL.cpp:26:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:27:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:35:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:35:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:35:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:36:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:36:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/Posix.cpp:59:64: warning: unused variable 'KERN_ABORTED' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

