# HydrogenVM report — f63368e

2026-10-27T21:05:01+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 20.4s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  /Users/runner/work/HydrogenVM/HydrogenVM/src/runtime/MemoryDiagnostics.cpp:24:26: error: 'os_proc_available_memory' is unavailable: not available on macOS
  1 error generated.
  ```

