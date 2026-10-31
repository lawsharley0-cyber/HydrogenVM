# HydrogenVM report — 4930e8a

2026-10-31T03:17:41+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 38.7s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:1030:15: error: no member named 'Hydrogen' in namespace 'Hydrogen::Runtime'; did you mean simply 'Hydrogen'?
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/Bundle.cpp:1083:15: error: no member named 'Hydrogen' in namespace 'Hydrogen::Runtime'; did you mean simply 'Hydrogen'?
  2 errors generated.
  ```

