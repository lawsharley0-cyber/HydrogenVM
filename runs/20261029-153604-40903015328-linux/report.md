# HydrogenVM report — 82a2808

2026-10-29T15:35:22+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 7.0.0-1017-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 42.1s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/../AssetCache.hpp:13:25: error: unknown type name 'Process'; did you mean 'Hydrogen::Runtime::Process'?
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/foundation/../AssetCache.hpp:32:34: error: unknown type name 'Process'
  2 errors generated.
  ```

