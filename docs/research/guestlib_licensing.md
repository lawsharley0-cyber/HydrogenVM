# Guest C++ Runtime Libraries — Licensing & Distribution Note (S3)

**Date:** 2026-11-03  
**Initiative:** Guest-Dylib Initiative (`Refs #311`)  
**Status:** Owner review required before bundling any built `.dylib` in a distributed HydrogenVM `.ipa`.

---

## 1. Purpose & Build Provenance

HydrogenVM's Guest-Dylib experiment (`ProcessConfig::guestLibs`) loads 32-bit ARMv7 iOS (`MH_DYLIB`) builds of C++ runtime libraries into guest memory in place of host-side per-symbol C++ stand-ins.

Per repository policy (`AGENTS.md` and `tools/STUB_HONESTY.md`):
- Built `.dylib` binaries are **never committed to git**.
- `tools/guestlibs/build_guestlibs.py` builds them reproducibly into an external output directory (`--out <dir>`) or private fixture store (`HVM_FIXTURE_ROOT`).
- **Build modes & provenance:**
  1. **Hermetic freestanding runtime build (default offline mode):** Because CI and agent sandboxes do not download multi-hundred-megabyte external source archives over the network during test runs, `build_guestlibs.py` compiles a freestanding ARMv7 C/ASM runtime implementation covering the core allocator (`operator new`/`delete` via `_malloc`/`_free`), exception storage (`__cxa_allocate_exception`/`__cxa_free_exception` via `_calloc`/`_free`), thread-safe static initialization guards (`__cxa_guard_acquire`/`release`/`abort`), SjLj unwind frame registration (`_Unwind_SjLj_Register`/`Unregister`), and 32/64-bit integer arithmetic/bitops (`__divsi3`, `__udivsi3`, `__modsi3`, `__umodsi3`, `__clzsi2`, `__ctzsi2`). Any remaining ABI export that is not yet wired to a full upstream translation unit traps loudly via `_abort()` (`udf #0`) rather than returning fake success.
  2. **Full upstream source build (`--source-root <dir>`):** When pre-staged, SHA-256-verified upstream tarballs (`libstdcxx-56` / `gcc-4.2.1` and `llvmorg-9.0.1` `compiler-rt`, `libcxxabi`, `libcxx`) are supplied via `--source-root`, the resulting binaries derive directly from those upstream sources and carry the upstream licensing obligations detailed below.
- Shipping any third-party runtime binary inside an iOS `.ipa` bundle requires an explicit licensing decision from the project owner.

---

## 2. Target Libraries & Licences

| Target Group | Binary (`LC_ID_DYLIB`) | Upstream Source | Licence |
|---|---|---|---|
| **Target 1 (GCC 4.2.1 / Apple `libstdcxx`)** | `/usr/lib/libgcc_s.1.dylib` | GCC `libgcc` (Apple `libstdcxx-56` / `gcc-4.2.1`) + LLVM `compiler-rt` (`llvmorg-9.0.1`) | `GPL-2.0-or-later WITH GCC-exception-2.0` (and `Apache-2.0 WITH LLVM-exception` for `compiler-rt` helpers) |
| **Target 1 (GCC 4.2.1 / Apple `libstdcxx`)** | `/usr/lib/libstdc++.6.dylib` | GNU `libstdc++-v3` (Apple `libstdcxx-56` / `gcc-4.2.1`) | `GPL-2.0-or-later WITH GCC-exception-2.0` (note: GCC 4.2.1 was the final GPLv2 release of GCC prior to the GPLv3 / `GCC-exception-3.1` transition in GCC 4.2.2 / 4.4) |
| **Target 2 (LLVM 9.0.1)** | `/usr/lib/libc++abi.dylib` | LLVM `libcxxabi` (`llvmorg-9.0.1`) | `Apache-2.0 WITH LLVM-exception` (dual-licensed with UIUC/NCSA in 9.0.1) |
| **Target 2 (LLVM 9.0.1)** | `/usr/lib/libc++.1.dylib` | LLVM `libcxx` (`llvmorg-9.0.1`) | `Apache-2.0 WITH LLVM-exception` (dual-licensed with UIUC/NCSA in 9.0.1) |

---

## 3. Distribution & Attribution Obligations for an iOS App Bundle

### 3.1 Target 2: `libc++.1.dylib` and `libc++abi.dylib` (`Apache-2.0 WITH LLVM-exception`)

- **Permissive binary redistribution:** `Apache-2.0 WITH LLVM-exception` (and the legacy UIUC/NCSA license) permits redistributing compiled `.dylib` binaries inside a proprietary or open-source iOS `.ipa` bundle without copyleft or source-disclosure obligations on HydrogenVM itself.
- **Required obligations if shipped in `HydrogenVM.ipa`:**
  1. Include the full text of the `Apache-2.0 WITH LLVM-exception` (and UIUC/NCSA) license in HydrogenVM's third-party notices (`Settings -> Acknowledgements` or bundled `THIRD_PARTY_NOTICES.txt`).
  2. Retain upstream copyright notices from the LLVM Project.
  3. State any build modifications made by `tools/guestlibs/build_guestlibs.py` for ARMv7 iOS cross-compilation.

### 3.2 Target 1: `libstdc++.6.dylib` and `libgcc_s.1.dylib` (`GPL-2.0-or-later WITH GCC-exception-2.0`)

- **GCC Linking Exception scope:** In GCC 4.2.1 (`GPL-2.0-or-later WITH GCC-exception-2.0`), the special exception in the `libstdc++-v3` and `libgcc` headers permits linking compiled code with the library to produce an executable without causing the resulting executable to be covered by the GNU GPL. (Later GCC 4.4+ releases use `GPL-3.0-or-later WITH GCC-exception-3.1`, the GCC Runtime Library Exception.)
- **Copyleft / Corresponding Source obligations when distributing upstream-built `.dylib` files:**
  1. While the linking exception shields the host emulator (`HydrogenVM`) and the guest `.ipa` from GPL copyleft propagation, distributing binary copies of upstream-built `libstdc++.6.dylib` and `libgcc_s.1.dylib` inside `HydrogenVM.ipa` triggers **GPLv2 Section 3** (or **GPLv3 Section 6** if a later GCC tarball is used) obligations **for the `libstdc++` and `libgcc` libraries themselves**:
     - Include a copy of the applicable GPL and GCC linking exception text in the app bundle / UI acknowledgements.
     - Provide the Corresponding Source (the exact pinned upstream source archive + `tools/guestlibs/build_guestlibs.py` build scripts and flags) alongside the binary distribution or via a durable written offer / download link.
     - Note: These Corresponding Source obligations apply only when distributing `.dylib` binaries compiled from the upstream GCC/`libstdcxx` sources (`--source-root`), not to the self-contained freestanding runtime generated in offline test mode.
  2. **App Store vs Sideloaded `.ipa` consideration:** Apple's App Store distribution terms impose DRM/re-distribution restrictions that conflict with strict GPL Section 6/7 terms unless binaries are kept in an external user/developer fixture store (`HVM_FIXTURE_ROOT`), whereas sideloaded unsigned `.ipa` distribution with full source access in the GitHub repository satisfies Corresponding Source availability if the exact tarball used to produce `libstdc++.6.dylib` / `libgcc_s.1.dylib` is archived and mirrored alongside the release.

---

## 4. Owner Decision Gate

> **Escalation to Owner:**
> 1. **Target 2 (`libc++.1.dylib` + `libc++abi.dylib`, `Apache-2.0 WITH LLVM-exception`):** Low legal friction; requires only standard attribution in third-party notices if S6 passes the technical gate.
> 2. **Target 1 (`libstdc++.6.dylib` + `libgcc_s.1.dylib`, `GPL-2.0-or-later WITH GCC-exception-2.0`):** Requires owner sign-off on whether to (a) ship upstream-built `libstdc++.6.dylib` / `libgcc_s.1.dylib` in sideloaded releases with full GPLv2 + GCC-exception-2.0 notice and mirrored Corresponding Source, (b) keep Target 1 external in the user's private store (`HVM_FIXTURE_ROOT`), or (c) implement an `Apache-2.0 WITH LLVM-exception` ABI compatibility shim layered over `libc++abi` for legacy `_ZNSs*` / `_ZNSt8ios_base*` symbols.
