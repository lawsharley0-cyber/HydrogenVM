# HydrogenVM report — 457a85f

2026-10-24T01:58:33+00:00 · AMD EPYC 7763 64-Core Processor (x86_64, Linux 6.17.0-1022-azure)

**1 test(s) failed.**

| Suite | Passed | Failed | Skipped | Time |
|---|---:|---:|---:|---:|
| build | 0 | 1 | 0 | 38.9s |

## Problems (1)

### Failures (1)

- **build / ninja**

  ```
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/Process.cpp:242:19: error: redefinition of 'digest' with a different type: 'unsigned char[32]' vs 'const std::vector<uint8_t>' (aka 'const vector<unsigned char>')
  /home/runner/work/HydrogenVM/HydrogenVM/src/runtime/Process.cpp:246:5: error: no matching function for call to 'SHA256'
  2 errors generated.
  ```

