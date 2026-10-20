# HydrogenVM report — ceba289

2026-10-20T15:27:12+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**7 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 1 | 0 | 0 | 220.3s |
| cpu | 15 | 0 | 0 | 0.3s |
| ios | 0 | 6 | 0 | 0.0s |
| bench | 4 | 1 | 0 | 0.5s |

## Speed


Raw interpreter speed (CPU test programs): ctests_thumb 442 MIPS, ctests_arm 462 MIPS, ftests_vfp_thumb_softfp 431 MIPS, ftests_neon_thumb 379 MIPS

## Problems (13)

### Failures (7)

- **ios / CTest**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **ios / CTestLegacy**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **ios / BinPlist**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **ios / ObjCTest**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **ios / FoundationTest**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **ios / UIKitTest**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```
- **bench / coremark**

  ```
  build failed:
  Traceback (most recent call last):
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 191, in <module>
      main()
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 188, in main
      build_app(a)
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 134, in build_app
      syms = undefined_symbols(objs)
             ^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 66, in undefined_symbols
      out = run([NM, "-u", "--just-symbol-name"] + objs)
            ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/home/runner/work/HydrogenVM/HydrogenVM/tools/ios_build.py", line 39, in run
      p = subprocess.run(cmd, capture_output=True, text=True, **kw)
          ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 548, in run
      with Popen(*popenargs, **kwargs) as process:
           ^^^^^^^^^^^^^^^^^^^^^^^^^^^
    File "/usr/lib/python3.12/subprocess.py", line 1026, in __init__
      self._execute_child(args, executable, preexec_fn, close_fds,
    File "/usr/lib/python3.12/subprocess.py", line 1955, in _execute_child
      raise child_exception_type(errno_num, err_msg, err_filename)
  FileNotFoundError: [Errno 2] No such file or directory: 'llvm-nm'
  ```

### Warnings (6)

- **build / compiler**: src/runtime/foundation/NSNumber.cpp:142:9: warning: variable 'depth' set but not used [-Wunused-but-set-variable]
- **build / compiler**: src/runtime/objc/Runtime.cpp:9:46: warning: unused variable 'RO_HAS_CXX_STRUCTORS' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:27:17: warning: unused variable 'kPi' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/CoreGraphics.cpp:35:6: warning: unused function 'obj_dealloc' [-Wunused-function]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:14:60: warning: unused variable 'kP' [-Wunused-const-variable]
- **build / compiler**: src/runtime/uikit/UIControls.cpp:17:4: warning: unused function 'allocInit' [-Wunused-function]

