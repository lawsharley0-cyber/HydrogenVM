# HydrogenVM report — 54d242a

2026-10-29T15:47:21+00:00 · Intel(R) Xeon(R) Processor @ 2.30GHz (x86_64, Linux 6.8.0)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 18.8s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:123:13: error: expected expression
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:123:21: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:123:22: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:123:53: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:123:54: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:37: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:38: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:53: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:54: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:55: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:56: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:57: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:58: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:59: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:136:60: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:138:4: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:138:5: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:138:6: error: source file is not valid UTF-8
  /usr/bin/../lib/gcc/x86_64-linux-gnu/13/../../../../include/c++/13/string_view:138:7: error: source file is not valid UTF-8
  fatal error: too many errors emitted, stopping now [-ferror-limit=]
  clang++: error: clang frontend command failed with exit code 139 (use -v to see invocation)
  clang++: error: unable to execute command: Segmentation fault
  ```
