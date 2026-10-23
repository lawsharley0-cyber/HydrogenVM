# HydrogenVM report — e54811d

2026-10-23T22:30:49+00:00 · Apple M1 (Virtual) (arm64, Darwin 25.6.0)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 12.9s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  /Users/runner/work/HydrogenVM/HydrogenVM/src/core/cpu/Engine.cpp:111:21: error: no member named 'write' in the global namespace; did you mean simply 'write'?
  1 error generated.
  ```

