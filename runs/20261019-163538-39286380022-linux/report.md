# HydrogenVM report — cc6156e

2026-10-19T16:35:16+00:00 · Intel(R) Xeon(R) 6973P-C (x86_64, Linux 6.17.0-1022-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 21.9s |

## Problems (5)

### Failures (1)

- **build / ninja**

  ```
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:752:10: error: no member named 'sort' in namespace 'std'
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:778:10: error: no member named 'sort' in namespace 'std'
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:877:10: error: no member named 'sort' in namespace 'std'
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:1055:83: error: too many arguments to function call, expected single argument '__filename', have 3 arguments
  4 warnings and 4 errors generated.
  ```

### Warnings (4)

- **build / compiler**: src/runtime/foundation/Bundle.cpp:146:144: warning: '&&' within '||' [-Wlogical-op-parentheses]
- **build / compiler**: src/runtime/foundation/Bundle.cpp:146:84: warning: '&&' within '||' [-Wlogical-op-parentheses]
- **build / compiler**: src/runtime/objc/ObjC.hpp:148:43: warning: expression with side effects will be evaluated despite being used as an operand to 'typeid' [-Wpotentially-evaluated-expression]
- **build / compiler**: src/runtime/objc/ObjC.hpp:148:57: warning: expression with side effects will be evaluated despite being used as an operand to 'typeid' [-Wpotentially-evaluated-expression]

