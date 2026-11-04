# Guest C++ Runtime Libraries Build Tooling (`tools/guestlibs/`)

Reproducible cross-compilation pipeline for the 32-bit ARMv7 iOS (`MH_DYLIB`) C++ runtime libraries used by HydrogenVM's `guestLibs` experiment (`Refs #311`):

1. **Target 1 (older games — GCC 4.2.1 / Apple `libstdcxx` ABI):**
   - `libgcc_s.1.dylib` (`LC_ID_DYLIB`: `/usr/lib/libgcc_s.1.dylib`)
   - `libstdc++.6.dylib` (`LC_ID_DYLIB`: `/usr/lib/libstdc++.6.dylib`, links `/usr/lib/libgcc_s.1.dylib` and `/usr/lib/libSystem.B.dylib`)
2. **Target 2 (newer 32-bit games — LLVM `libc++` / `libc++abi` ABI):**
   - `libc++abi.dylib` (`LC_ID_DYLIB`: `/usr/lib/libc++abi.dylib`, links `/usr/lib/libSystem.B.dylib`)
   - `libc++.1.dylib` (`LC_ID_DYLIB`: `/usr/lib/libc++.1.dylib`, links `/usr/lib/libc++abi.dylib` and `/usr/lib/libSystem.B.dylib`)

## Usage

```bash
python3 tools/guestlibs/build_guestlibs.py --out /tmp/hvm-guestlibs
```

- Output `.dylib` binaries and `manifest.json` are written **only** to the directory passed via `--out` (never into the git repository).
- Compiler flags:
  - `--target=armv7-apple-ios6.0 -arch armv7 -miphoneos-version-min=6.0 -mfloat-abi=softfp -mfpu=neon -fno-stack-protector -fno-blocks -fPIC -O2 -g0`
- Linker flags (`arm-apple-darwin11-ld` / `ld64`):
  - `-arch armv7 -ios_version_min 6.0 -dylib -install_name /usr/lib/<name> -compatibility_version 1.0.0 -current_version 1.0.0`

## Licensing

See [`docs/research/guestlib_licensing.md`](../../docs/research/guestlib_licensing.md) for the full licence analysis (`GPL-2.0-or-later WITH GCC-exception-2.0` and `Apache-2.0 WITH LLVM-exception`) and owner decision gate before shipping any `.dylib` inside a distributed `.ipa`.
