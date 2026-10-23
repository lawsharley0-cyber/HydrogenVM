# HydrogenVM report — 187d01c

2026-10-23T15:11:59+00:00 · AMD EPYC 9V45 96-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**7 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 101.9s |
| cpu | 15 | 0 | 0 | 0.2s |
| ios | 0 | 6 | 0 | 0.0s |
| bench | 4 | 1 | 0 | 0.2s |

## Speed


Raw interpreter speed (CPU test programs): ctests_thumb 884 MIPS, ctests_arm 958 MIPS, ftests_vfp_thumb_softfp 864 MIPS, ftests_neon_thumb 799 MIPS

## Problems (23)

### Failures (7)

- **ios / CTest**

  ```
  build failed:
  rk/CTest/main.c.o: name 'struct' is not defined
  /home/runner/cctools/bin/arm-apple-darwin11-ld -arch armv7 -ios_version_min 6.0 -syslibroot /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CTest/sdk -o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/CTest.app/CTest -pie /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CTest/main.c.o -lSystem
  Undefined symbols for architecture armv7:
    "___error", referenced from:
        _main in main.c.o
    "___stack_chk_fail", referenced from:
        _main in main.c.o
    "___stack_chk_guard", referenced from:
        _main in main.c.o
    "___stderrp", referenced from:
        _main in main.c.o
    "_atexit", referenced from:
        _main in main.c.o
    "_atof", referenced from:
        _main in main.c.o
    "_atoi", referenced from:
        _main in main.c.o
    "_bsearch", referenced from:
        _main in main.c.o
    "_calloc", referenced from:
        _main in main.c.o
    "_fclose", referenced from:
        _main in main.c.o
    "_feof", referenced from:
        _main in main.c.o
    "_fgets", referenced from:
        _main in main.c.o
    "_fopen", referenced from:
        _main in main.c.o
    "_fprintf", referenced from:
        _main in main.c.o
    "_free", referenced from:
        _main in main.c.o
    "_fseek", referenced from:
        _main in main.c.o
    "_ftell", referenced from:
        _main in main.c.o
    "_fwrite", referenced from:
        _main in main.c.o
    "_getenv", referenced from:
        _main in main.c.o
    "_longjmp", referenced from:
        _dive in main.c.o
    "_malloc", referenced from:
        _main in main.c.o
    "_memcmp", referenced from:
        _main in main.c.o
    "_memset", referenced from:
        _main in main.c.o
    "_printf", referenced from:
        _main in main.c.o
    "_pthread_create", referenced from:
        _main in main.c.o
    "_pthread_join", referenced from:
        _main in main.c.o
    "_pthread_mutex_lock", referenced from:
        _worker in main.c.o
    "_pthread_mutex_unlock", referenced from:
        _worker in main.c.o
  ```
- **ios / CTestLegacy**

  ```
  build failed:
  version_min 3.0 -syslibroot /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CTestLegacy/sdk -o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/CTestLegacy.app/CTestLegacy /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CTestLegacy/sdk/_src/crt1.o -e start /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CTestLegacy/main.c.o -lSystem
  Undefined symbols for architecture armv7:
    "___error", referenced from:
        _main in main.c.o
    "___stack_chk_fail", referenced from:
        _main in main.c.o
    "___stack_chk_guard", referenced from:
        _main in main.c.o
    "___stderrp", referenced from:
        _main in main.c.o
    "_atexit", referenced from:
        _main in main.c.o
    "_atof", referenced from:
        _main in main.c.o
    "_atoi", referenced from:
        _main in main.c.o
    "_bsearch", referenced from:
        _main in main.c.o
    "_calloc", referenced from:
        _main in main.c.o
    "_fclose", referenced from:
        _main in main.c.o
    "_feof", referenced from:
        _main in main.c.o
    "_fgets", referenced from:
        _main in main.c.o
    "_fopen", referenced from:
        _main in main.c.o
    "_fprintf", referenced from:
        _main in main.c.o
    "_free", referenced from:
        _main in main.c.o
    "_fseek", referenced from:
        _main in main.c.o
    "_ftell", referenced from:
        _main in main.c.o
    "_fwrite", referenced from:
        _main in main.c.o
    "_getenv", referenced from:
        _main in main.c.o
    "_longjmp", referenced from:
        _dive in main.c.o
    "_malloc", referenced from:
        _main in main.c.o
    "_memcmp", referenced from:
        _main in main.c.o
    "_memset", referenced from:
        _main in main.c.o
    "_printf", referenced from:
        _main in main.c.o
    "_pthread_create", referenced from:
        _main in main.c.o
    "_pthread_join", referenced from:
        _main in main.c.o
    "_pthread_mutex_lock", referenced from:
        _worker in main.c.o
    "_pthread_mutex_unlock", referenced from:
        _worker in main.c.o
    "_pthread_once", referenced from:
  ```
- **ios / BinPlist**

  ```
  build failed:
  n.c.o: name 'struct' is not defined
  /home/runner/cctools/bin/arm-apple-darwin11-ld -arch armv7 -ios_version_min 6.0 -syslibroot /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/BinPlist/sdk -o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/BinPlist.app/BinPlist -pie /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/BinPlist/main.c.o -lSystem
  Undefined symbols for architecture armv7:
    "___error", referenced from:
        _main in main.c.o
    "___stack_chk_fail", referenced from:
        _main in main.c.o
    "___stack_chk_guard", referenced from:
        _main in main.c.o
    "___stderrp", referenced from:
        _main in main.c.o
    "_atexit", referenced from:
        _main in main.c.o
    "_atof", referenced from:
        _main in main.c.o
    "_atoi", referenced from:
        _main in main.c.o
    "_bsearch", referenced from:
        _main in main.c.o
    "_calloc", referenced from:
        _main in main.c.o
    "_fclose", referenced from:
        _main in main.c.o
    "_feof", referenced from:
        _main in main.c.o
    "_fgets", referenced from:
        _main in main.c.o
    "_fopen", referenced from:
        _main in main.c.o
    "_fprintf", referenced from:
        _main in main.c.o
    "_free", referenced from:
        _main in main.c.o
    "_fseek", referenced from:
        _main in main.c.o
    "_ftell", referenced from:
        _main in main.c.o
    "_fwrite", referenced from:
        _main in main.c.o
    "_getenv", referenced from:
        _main in main.c.o
    "_longjmp", referenced from:
        _dive in main.c.o
    "_malloc", referenced from:
        _main in main.c.o
    "_memcmp", referenced from:
        _main in main.c.o
    "_memset", referenced from:
        _main in main.c.o
    "_printf", referenced from:
        _main in main.c.o
    "_pthread_create", referenced from:
        _main in main.c.o
    "_pthread_join", referenced from:
        _main in main.c.o
    "_pthread_mutex_lock", referenced from:
        _worker in main.c.o
    "_pthread_mutex_unlock", referenced from:
        _worker in main.c.o
  ```
- **ios / ObjCTest**

  ```
  build failed:
  ferenced from:
        -[Animal weakFriend] in main.m.o
        -[Forwarder forwardingTargetForSelector:] in main.m.o
    "_objc_destroyWeak", referenced from:
        -[Animal .cxx_destruct] in main.m.o
        _main in main.m.o
    "_objc_getAssociatedObject", referenced from:
        _main in main.m.o
    "_objc_getClass", referenced from:
        _main in main.m.o
    "_objc_getProperty", referenced from:
        -[Animal atomicObj] in main.m.o
        -[Forwarder target] in main.m.o
    "_objc_initWeak", referenced from:
        _main in main.m.o
    "_objc_loadWeakRetained", referenced from:
        -[Animal weakFriend] in main.m.o
        _main in main.m.o
    "_objc_msgSend", referenced from:
        +[Animal initialize] in main.m.o
        -[CopyMe copyWithZone:] in main.m.o
        -[Forwarder forwardingTargetForSelector:] in main.m.o
        _main in main.m.o
    "_objc_msgSendSuper2", referenced from:
        -[Animal init] in main.m.o
        -[Animal dealloc] in main.m.o
        -[Dog greet] in main.m.o
        -[Dog speedFor:] in main.m.o
        -[Tracker dealloc] in main.m.o
    "_objc_registerClassPair", referenced from:
        _main in main.m.o
    "_objc_release", referenced from:
        _main in main.m.o
        ___destroy_helper_block_e4_20s in main.m.o
    "_objc_retain", referenced from:
        _main in main.m.o
        ___copy_helper_block_e4_20s in main.m.o
    "_objc_retainAutoreleasedReturnValue", referenced from:
        -[Forwarder forwardingTargetForSelector:] in main.m.o
        _main in main.m.o
    "_objc_retainBlock", referenced from:
        _main in main.m.o
    "_objc_setAssociatedObject", referenced from:
        _main in main.m.o
    "_objc_setProperty_atomic", referenced from:
        -[Animal setAtomicObj:] in main.m.o
        -[Forwarder setTarget:] in main.m.o
    "_objc_setProperty_nonatomic_copy", referenced from:
        -[Animal setCopied:] in main.m.o
    "_objc_storeStrong", referenced from:
        -[Animal setFriendObj:] in main.m.o
        -[Animal .cxx_destruct] in main.m.o
        -[Holder hold:] in main.m.o
        -[Holder .cxx_destruct] in main.m.o
        -[Forwarder .cxx_destruct] in main.m.o
    "_objc_storeWeak", referenced from:
        -[Animal setWeakFriend:] in main.m.o
    "_objc_sync_enter", referenced from:
        _main in main.m.o
  ```
- **ios / FoundationTest**

  ```
  build failed:
  pDefaultMode", referenced from:
        _main in main.m.o
    "_kCFTypeArrayCallBacks", referenced from:
        _main in main.m.o
    "_kCFTypeDictionaryKeyCallBacks", referenced from:
        _main in main.m.o
    "_kCFTypeDictionaryValueCallBacks", referenced from:
        _main in main.m.o
    "_memcmp", referenced from:
        _main in main.m.o
    "_objc_autoreleasePoolPop", referenced from:
        -[Pinger background:] in main.m.o
        _main in main.m.o
    "_objc_autoreleasePoolPush", referenced from:
        -[Pinger background:] in main.m.o
        _main in main.m.o
    "_objc_autoreleaseReturnValue", referenced from:
        -[Person description] in main.m.o
    "_objc_enumerationMutation", referenced from:
        _main in main.m.o
    "_objc_msgSend", referenced from:
        -[Person description] in main.m.o
        -[Person encodeWithCoder:] in main.m.o
        -[Person initWithCoder:] in main.m.o
        -[Pinger tick:] in main.m.o
        -[Pinger later:] in main.m.o
        -[Pinger note:] in main.m.o
        -[Pinger onMain:] in main.m.o
        ...
    "_objc_msgSendSuper2", referenced from:
        -[Person initWithCoder:] in main.m.o
    "_objc_msgSend_stret", referenced from:
        _main in main.m.o
    "_objc_release", referenced from:
        -[Person description] in main.m.o
        -[Person encodeWithCoder:] in main.m.o
        -[Person initWithCoder:] in main.m.o
        -[Pinger note:] in main.m.o
        -[Pinger background:] in main.m.o
        _main in main.m.o
        ___testNumbersAndCollections_block_invoke in main.m.o
        ...
    "_objc_retain", referenced from:
        -[Person encodeWithCoder:] in main.m.o
        -[Person initWithCoder:] in main.m.o
        -[Pinger background:] in main.m.o
        _main in main.m.o
        ___testNumbersAndCollections_block_invoke in main.m.o
        ___testNumbersAndCollections_block_invoke.389 in main.m.o
        ___copy_helper_block_e4_20s24r in main.m.o
        ...
    "_objc_retainAutoreleasedReturnValue", referenced from:
        -[Person description] in main.m.o
        -[Person encodeWithCoder:] in main.m.o
        -[Person initWithCoder:] in main.m.o
        -[Pinger note:] in main.m.o
        _main in main.m.o
        ___testConcurrency_block_invoke in main.m.o
    "_objc_setProperty_nonatomic_copy", referenced from:
  ```
- **ios / UIKitTest**

  ```
  build failed:
  ate runSynchronousChecks] in main.m.o
    "_glReadPixels", referenced from:
        -[AppDelegate runSynchronousChecks] in main.m.o
    "_objc_autoreleasePoolPop", referenced from:
        _main in main.m.o
    "_objc_autoreleasePoolPush", referenced from:
        _main in main.m.o
    "_objc_enumerationMutation", referenced from:
        -[AppDelegate runSynchronousChecks] in main.m.o
    "_objc_msgSend", referenced from:
        -[DrawView drawRect:] in main.m.o
        -[LayoutView didMoveToWindow] in main.m.o
        -[TapView touchesBegan:withEvent:] in main.m.o
        -[TestViewController loadView] in main.m.o
        -[TestViewController viewDidLoad] in main.m.o
        -[TestViewController tapped:] in main.m.o
        -[TestViewController animationDidStop:finished:context:] in main.m.o
        ...
    "_objc_msgSendSuper2", referenced from:
        -[LayoutView layoutSubviews] in main.m.o
        -[TestViewController viewDidLoad] in main.m.o
        -[TestViewController viewWillAppear:] in main.m.o
    "_objc_msgSend_stret", referenced from:
        -[DrawView drawRect:] in main.m.o
        -[TapView touchesBegan:withEvent:] in main.m.o
        -[TestViewController loadView] in main.m.o
        -[AppDelegate runSynchronousChecks] in main.m.o
        -[AppDelegate application:didFinishLaunchingWithOptions:] in main.m.o
    "_objc_release", referenced from:
        -[DrawView drawRect:] in main.m.o
        -[LayoutView didMoveToWindow] in main.m.o
        -[TapView touchesBegan:withEvent:] in main.m.o
        -[TestViewController loadView] in main.m.o
        -[TestViewController viewDidLoad] in main.m.o
        -[AppDelegate runSynchronousChecks] in main.m.o
        ___destroy_helper_block_e4_20s in main.m.o
        ...
    "_objc_retain", referenced from:
        -[AppDelegate runSynchronousChecks] in main.m.o
        ___copy_helper_block_e4_20s in main.m.o
    "_objc_retainAutoreleasedReturnValue", referenced from:
        -[DrawView drawRect:] in main.m.o
        -[LayoutView didMoveToWindow] in main.m.o
        -[TapView touchesBegan:withEvent:] in main.m.o
        -[TestViewController loadView] in main.m.o
        -[TestViewController viewDidLoad] in main.m.o
        -[AppDelegate runSynchronousChecks] in main.m.o
        -[AppDelegate application:didFinishLaunchingWithOptions:] in main.m.o
        ...
    "_objc_storeStrong", referenced from:
        -[TestViewController setPanelLabel:] in main.m.o
        -[TestViewController setContainer:] in main.m.o
        -[TestViewController setChild:] in main.m.o
        -[TestViewController setButton:] in main.m.o
        -[TestViewController setGestureView:] in main.m.o
        -[TestViewController .cxx_destruct] in main.m.o
        -[AppDelegate setWindow:] in main.m.o
        ...
    "_printf", referenced from:
  ```
- **bench / coremark**

  ```
  build failed:
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_list_join.c.o: name 'struct' is not defined
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_main.c.o: name 'struct' is not defined
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_matrix.c.o: name 'struct' is not defined
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_state.c.o: name 'struct' is not defined
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_util.c.o: name 'struct' is not defined
  Warning parsing /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_portme.c.o: name 'struct' is not defined
  /home/runner/cctools/bin/arm-apple-darwin11-ld -arch armv7 -ios_version_min 6.0 -syslibroot /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/sdk -o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/CoreMark.app/CoreMark -pie /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_list_join.c.o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_main.c.o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_matrix.c.o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_state.c.o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_util.c.o /home/runner/work/HydrogenVM/HydrogenVM/build/ios/_work/CoreMark/core_portme.c.o -lSystem
  Undefined symbols for architecture armv7:
    "___stack_chk_fail", referenced from:
        _main in core_main.c.o
        _core_bench_state in core_state.c.o
    "___stack_chk_guard", referenced from:
        _main in core_main.c.o
        _core_bench_state in core_state.c.o
        _main in core_main.c.o
        _core_bench_state in core_state.c.o
    "___udivsi3", referenced from:
        _main in core_main.c.o
    "_clock_gettime", referenced from:
        _start_time in core_portme.c.o
        _stop_time in core_portme.c.o
    "_free", referenced from:
        _portable_free in core_portme.c.o
       (maybe you meant: _portable_free)
    "_malloc", referenced from:
        _portable_malloc in core_portme.c.o
       (maybe you meant: _portable_malloc)
    "_memset", referenced from:
        _core_init_state in core_state.c.o
    "_printf", referenced from:
        _main in core_main.c.o
    "_putchar", referenced from:
        _main in core_main.c.o
    "_puts", referenced from:
        _main in core_main.c.o
  ld: symbol(s) not found for architecture armv7
  ```

### Warnings (16)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:23:47: warning: unused variable 'kDrawFramebuffer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:24:54: warning: unused variable 'kDrawFramebufferBinding' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:20: warning: unused variable 'kVertexArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:50: warning: unused variable 'kNormalArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:32:80: warning: unused variable 'kColorArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:20: warning: unused variable 'kTexCoordArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/gl/HostGL.cpp:33:52: warning: unused variable 'kPointSizeArrayPointer' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxx.cpp:38:40: warning: unused variable 'kBinary' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:51:35: warning: unused variable 'kIosSize' [-Wunused-const-variable]
- **build / compiler**: src/runtime/libc/LibStdcxxStreams.cpp:57:60: warning: unused variable 'mTrunc' [-Wunused-const-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:28:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:36:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

