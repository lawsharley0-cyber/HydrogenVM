# HydrogenVM report — 752f91b

2026-10-24T00:58:21+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 218.6s |
| cpu | 15 | 0 | 0 | 0.8s |
| ios | 5 | 1 | 0 | 1.0s |
| bench | 5 | 0 | 0 | 24.5s |

## Speed

CoreMark **1708.9** iterations/s (575.5 guest MIPS)

| Original device | CPU | Device CoreMark (est.) | Emulator | Effective clock |
|---|---|---:|---:|---:|
| iPhone 3GS | Cortex-A8 | 1440 | **118.7%** | 712 MHz |
| iPhone 4 | Apple A4 (Cortex-A8) | 1920 | **89.0%** | 712 MHz |
| iPhone 4S | Apple A5 (Cortex-A9) | 2400 | **71.2%** | 570 MHz |
| iPhone 5 ← goal | Apple A6 (Swift) | 4550 | **37.6%** | 488 MHz |

Raw interpreter speed (CPU test programs): ctests_thumb 470 MIPS, ctests_arm 504 MIPS, ftests_vfp_thumb_softfp 450 MIPS, ftests_neon_thumb 401 MIPS

| App | Wall | Frames | Avg frame | Max frame | Slow frames |
|---|---:|---:|---:|---:|---:|
| CTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| CTestLegacy | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| BinPlist | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |
| ObjCTest | 0.01s | 0 | 0.00 ms | 0.00 ms | 0 |
| FoundationTest | 0.65s | 0 | 0.00 ms | 0.00 ms | 0 |
| UIKitTest | 0.02s | 0 | 0.00 ms | 0.00 ms | 0 |

## Problems (33)

### Failures (1)

- **ios / UIKitTest**

  ```
  exit 70 (want 0); expected line missing
  ok transformed frame
  ok identity transform restores frame
  ok autoresizing (flexible width + top margin)
  ok setNeedsLayout + layoutIfNeeded
  ok bringSubviewToFront
  ok UIColor components
  ok UIColor CGColor
  ok UIFont metrics
  ok sizeWithFont:
  ok sizeWithFont:constrainedToSize: wraps
  ok imageNamed: picks @2x
  ok CGImage of @2x image
  ok imageNamed: missing returns nil
  ok CgBI (Xcode-crushed) PNG decodes
  ok CgBI channel order
  ok UIGraphicsGetImageFromCurrentImageContext
  ok UIRectFill pixels
  ok drawInRect: pixels (top-left origin)
  ok untouched pixels stay clear
  ok PNG encode/decode round trip
  ok CGBitmapContext y-up fill
  ok CGPathContainsPoint
  ok CGRectIntersection
  ok NSStringFromCGRect / CGRectFromString
  ok NSValue CGRect
  --- stderr ---
  [hvm:error] Stopped: -[NSBundle loadNibNamed:owner:options:]: Could not load NIB in bundle: 'Panel'
    pc [HydrogenVM] objc_msgSend
    lr 0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
    r0  100003a0  r1  0e018fec  r2  00010204  r3  10010100
    r4  100003a0  r5  10010100  r6  10000200  r7  beffff68
    r8  0000f1d6  r9  00000000  r10 00010204  r11 00000000
    r12 000100e0  r13 befffbc0  r14 0000c251  r15 ffff0c7c
    backtrace:
      [HydrogenVM] objc_msgSend
      0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
      0x0000e1f7 UIKitTest+41462 -[AppDelegate application:didFinishLaunchingWithOptions:] + 0x1e2
      [HydrogenVM] _hvm_return_to_host
      [HydrogenVM] _hvm_return_to_host
  ```

### Crashes (1)

- **ios / UIKitTest**

  ```
  Stopped: -[NSBundle loadNibNamed:owner:options:]: Could not load NIB in bundle: 'Panel'
    pc [HydrogenVM] objc_msgSend
    lr 0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
    r0  100003a0  r1  0e018fec  r2  00010204  r3  10010100
    r4  100003a0  r5  10010100  r6  10000200  r7  beffff68
    r8  0000f1d6  r9  00000000  r10 00010204  r11 00000000
    r12 000100e0  r13 befffbc0  r14 0000c251  r15 ffff0c7c
    backtrace:
      [HydrogenVM] objc_msgSend
      0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
      0x0000e1f7 UIKitTest+41462 -[AppDelegate application:didFinishLaunchingWithOptions:] + 0x1e2
      [HydrogenVM] _hvm_return_to_host
      [HydrogenVM] _hvm_return_to_host
  ```

### Runtime errors (1)

- **ios / UIKitTest**

  ```
  Stopped: -[NSBundle loadNibNamed:owner:options:]: Could not load NIB in bundle: 'Panel'
    pc [HydrogenVM] objc_msgSend
    lr 0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
    r0  100003a0  r1  0e018fec  r2  00010204  r3  10010100
    r4  100003a0  r5  10010100  r6  10000200  r7  beffff68
    r8  0000f1d6  r9  00000000  r10 00010204  r11 00000000
    r12 000100e0  r13 befffbc0  r14 0000c251  r15 ffff0c7c
    backtrace:
      [HydrogenVM] objc_msgSend
      0x0000c251 UIKitTest+33360 -[AppDelegate runSynchronousChecks] + 0x2260
      0x0000e1f7 UIKitTest+41462 -[AppDelegate application:didFinishLaunchingWithOptions:] + 0x1e2
      [HydrogenVM] _hvm_return_to_host
      [HydrogenVM] _hvm_return_to_host
  ```

### Below speed goal (1)

- **bench / coremark**: 37.6% of iPhone 5 single-core speed (goal: 100%)

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
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]
- **ios / UIKitTest**: nib "Panel" not found in the app bundle

