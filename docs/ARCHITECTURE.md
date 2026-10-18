# HydrogenVM architecture

HydrogenVM runs 32-bit ARM (ARMv7-A, ARM and Thumb-2) guest code on 64-bit hosts
without generating any machine code at runtime, so it works under iOS's W^X
policy with no JIT entitlement.

```
 guest PC ──► Engine::enterBlock ──► block cache hit? ──► run Ops ──► exit op
                    │ miss                                          │
                    ▼                                               ├─ linked: tail-call next block
              decode one basic block                                ├─ budget used up: return to dispatcher
              (ARM or Thumb, lazily)                                └─ SVC / UDF / HLE / fault: return
```

## Source map

| Path | What it is |
|---|---|
| `src/core/memory/GuestArena.*` | 4 GB guest address space, commit tracking, software page permissions |
| `src/core/cpu/Cpu.hpp` | Guest register state, `Op` (IR instruction), `Block`, condition table |
| `src/core/cpu/Handlers.hpp` | Every IR handler: ALU, multiply, load/store, LDM/STM, exclusives, media ops, exits |
| `src/core/cpu/BlockBuilder.hpp` | Emits IR for one block; conditional execution, PC materialisation, dead-flag pass |
| `src/core/cpu/DecodeArm.cpp` | A32 decoder |
| `src/core/cpu/DecodeThumb.cpp` | T16 + T32 decoder (IT blocks resolved statically) |
| `src/core/cpu/FpHandlers.hpp` | VFPv3/v4 scalar floating point handlers |
| `src/core/cpu/NeonHandlers.hpp` | Advanced SIMD (NEON) handlers |
| `src/core/cpu/DecodeVfpNeon.cpp` | VFP decoder + Thumb→ARM translation of VFP/NEON encodings |
| `src/core/cpu/DecodeNeon.cpp` | NEON decoder |
| `src/core/cpu/Engine.*` | Block cache, dispatcher, HLE table, fault guard |
| `src/loader/ElfLoader.*` | ELF32 loader (tests now; Android `.so` in Phase 3) |
| `src/selftest/SelfTest.*` | Built-in smoke test + speed check (runs on device from the app) |
| `app/` | iPhone app (XcodeGen project, SwiftUI shell, ObjC++ bridge) |
| `tests/` | Asm unit tests, C differential tests, runner, benchmark |

## Changes from the original handoff, and why

| Handoff | Problem | Replacement |
|---|---|---|
| Static load-time pre-decoder | ARM binaries mix code with literal pools and jump tables, and ARM/Thumb state is only known at runtime (BX/BLX), so a static sweep decodes data as code and guesses wrong modes. | **Lazy block decoding**: decode a basic block the first time the guest jumps to it, in the state it jumps in, and cache it keyed by `address | thumb`. Still no JIT: the cache holds data (Ops), not code. |
| Branch offsets counted in IR nodes | Fusion breaks the 1-guest-insn-per-node mapping; indirect branches (`BX LR`, `POP {PC}`, `LDR PC`) had no way to find their target. | Blocks end in exit ops. Direct branches cache a pointer to the successor block ("block linking"); indirect branches look up the target address through a hashed fast cache. |
| Unbounded `musttail` chain | The guest could never be interrupted (threads, signals, UI events). | Every block exit subtracts the instructions it retired from a budget and returns to the dispatcher when it runs out. Tested by re-running every C test in 97-instruction slices. |
| r0–r2 pinned via `GET_REG`/`SET_REG` ternaries | Each operand access becomes 2–3 compare-and-branch sequences, slower than the L1 load it avoids. | Registers live in `Cpu::r[]`; handlers pin only `cpu`, `op` and the arena base. `preserve_none` is used when the compiler supports it on the target (clang ≥ 19). |
| `Op_Fused_CMP_BNE` | Immediate limited to 0–255 (`uint8_t rm`); never updated NZCV. | Replaced by general **dead-flag elimination**: backward liveness over N/Z/C/V per block; S-ops whose flags are never read switch to a no-flags handler, dead compares become no-ops. Flags are assumed live at exits, so it's always safe. |
| PC (r15) reads | Not handled. | The decoder knows each instruction's address, so PC reads become constants (+8 ARM, +4 Thumb, word-aligned for literals/ADR). |
| `Op_LDR_Imm` raw pointer cast | Undefined behaviour on unaligned loads (which ARMv7 allows). | All guest memory access uses `memcpy`/atomics. |
| `MapRegion` re-`mmap` with `MAP_FIXED` | iOS uses 16 KB host pages but 32-bit iOS binaries use 4 KB pages. Mapping a second segment that shares a host page wiped the first. | Commits are tracked per host page and never re-mapped; guest permissions are tracked in software at 4 KB granularity. |
| HLE via UDF trap table at `0xFFFF0000` | Needs trap instructions written into guest memory. | **HLE by address**: imports are bound to addresses in `0xFFFF0000–0xFFFFFFFF`; when execution reaches one, the engine builds a block that calls the host function and returns to LR. Nothing is decoded there. |
| "Zero-copy" bridging of 32-bit ObjC calls to 64-bit UIKit | 32-bit and 64-bit ABIs differ (pointer size, `NSInteger`, `CGFloat`, struct layout), so guest objects can't be handed to host frameworks as-is. | Planned: a guest-side Objective-C runtime with reimplemented framework classes, in the style of touchHLE. Graphics calls (GLES) still bridge to Metal on the host. |
| Faults | Not handled. | `SIGSEGV`/`SIGBUS` inside the arena unwind to `Engine::run` (`siglongjmp`; safe because every handler is a tail call) and report `Exit::MemFault` with the guest address. |

## Execution core

* **Op** (32 bytes): handler pointer, four byte-sized operand fields, two 32-bit
  immediates, the source instruction's guest address, and a mutable link slot.
* **Handlers** are templates specialised on the operation, whether flags are
  set, and the operand-2 shape, so the hot path has no runtime switches on
  instruction type. ~400 ALU variants are generated from one template.
* **Conditional execution**: a `CondSkip` op in front of the instruction's ops
  jumps over them when the condition fails. Thumb IT blocks use the same op;
  ITSTATE is tracked while decoding, so there's no runtime IT state.
* **Exclusives** (`LDREX`/`STREX` and B/H/D forms) are compare-and-swap against
  the value seen by `LDREX`, which stays correct when guest threads run on real
  host threads.
* **CP15 thread registers**: `MRC p15,0,Rt,c13,c0,3` (32-bit iOS reads its TSD
  pointer this way) reads `Cpu::tpidruro`.

## Instruction coverage (integer)

ARM and Thumb-2: all data-processing forms and shifts, MUL/MLA/MLS, all long
multiplies incl. UMAAL, halfword/dual/most-significant-word multiplies,
SDIV/UDIV, loads/stores in every addressing mode incl. LDRD/STRD, LDM/STM
(all modes, PC in list, base in list), LDREX/STREX family, CLREX, SWP, B/BL/BLX
(imm and register) with interworking, BX, CBZ/CBNZ, TBB/TBH, IT, MOVW/MOVT,
CLZ/RBIT/REV*, extends (incl. add and 16-bit forms), bitfields, SSAT/USAT
(and 16), QADD family, parallel add/subtract (S/Q/SH/U/UQ/UH), SEL, PKH,
USAD8/USADA8, MRS/MSR (APSR), barriers, hints, SVC, BKPT/UDF.

Not supported: privileged/system instructions (not reachable from iOS user
code) and SETEND (big-endian data; never used by iOS apps).

## Floating point and SIMD

**VFPv3/VFPv4**: all arithmetic incl. VNMLA/VNMLS/VNMUL and fused VFMA/VFMS/
VFNMA/VFNMS, VSQRT, VABS/VNEG, VMOV (imm/reg/core), VCMP{E} + VMRS APSR_nzcv,
conversions (f32↔f64, int↔float with RZ or FPSCR rounding, fixed point,
half precision VCVTB/VCVTT), VLDR/VSTR/VLDM/VSTM/VPUSH/VPOP, VMRS/VMSR.

**NEON**: the full ARMv7 Advanced SIMD integer and single-precision set —
3-same, 3-different (long/wide/narrow), by-scalar, 2-reg shifts (incl.
narrowing/rounding/saturating), 2-reg misc, modified immediates, VEXT, VTBL/
VTBX, VDUP, VZIP/VUZP/VTRN/VSWP, VREV, estimates (VRECPE/VRSQRTE + steps),
f16↔f32, and VLD1–4/VST1–4 in all multiple/single-lane/all-lanes forms.

Accuracy decisions:
* `fp contract(off)` everywhere: ARM VMLA rounds twice, and an arm64 host
  compiler would otherwise fuse `a*b + c` into one FMADD.
* NEON float is flush-to-zero with default NaN, as on ARMv7 hardware.
* VFP honours FPSCR.FZ and FPSCR.DN on a slow path taken only when a guest
  enables them; arithmetic rounds to nearest (FPSCR.RMode is honoured by VCVTR).
* VRECPE/VRSQRTE follow the ARM ARM estimate tables bit-exactly
  (e.g. `vrecpe(1.0) = 0.998046875`).
* FPSCR exception flags aren't tracked; QC (saturation) is.

## Decoder coverage fuzzing

`tools/decode_fuzz.py build/hvm_decodecheck 1000000` generates random ARM and
Thumb-2 encodings, uses `llvm-mc` to identify the valid user-mode ARMv7 ones
(+VFPv4/NEON/idiv), and reports any the decoder rejects. Current result:
**0 rejected out of ~544,000 valid encodings** (UNPREDICTABLE forms such as
PC-based LDM/STM and NEON loads are filtered out, and are intentionally rejected).

## Testing

`tools/run_tests.sh` builds everything and runs:

* **Asm unit tests** (`tests/asm/*.S`): hand-written edge cases with expected
  registers/flags (carry/overflow, shifts ≥ 32, RRX, IT corner cases, TBB/TBH,
  interworking, exclusives, unmapped-access faults, …).
* **C differential tests**, compiled for the host to get expected values and
  for the guest in several configurations, then run in HydrogenVM and compared.
  Each is also re-run in 97-instruction time slices to check that every block
  exit leaves a resumable state.
  * `tests/c/ctests.c` (27 integer programs): ARM/Thumb-2 × -O2/-Os, plus
    Thumb-2 with hardware divide.
  * `tests/c/ftests.c` + `vfast.c` (29 float/SIMD programs): VFP with hard and
    softfp ABIs (iOS uses softfp), and NEON builds at -O3/-Os where clang
    auto-vectorises the loops (~80 distinct NEON instruction forms).
* **VFP/NEON asm tests** (`tests/asm/vfp_neon.S`): fused vs unfused multiply-add,
  rounding modes, fixed point, half precision, VLD2/3/4, VTBL/VTBX, permutes,
  saturation + QC, estimates, scalar forms, and Thumb-2 encodings.
* **Decoder fuzzing** (above).

A reference CPU (e.g. Unicorn) for instruction-by-instruction fuzzing would be
a good next addition; it wasn't installable in the environment this was built in.

## Performance

`hvm_bench` on an x86-64 cloud VM (clang 18, so no `preserve_none`):
~320 MIPS on the integer suite, ~530 on VFP code, ~470 on NEON code (each NEON
instruction does more work). Apple Silicon numbers still need measuring — the
app's Diagnostics tab runs the self-test and reports MIPS on device.

Next optimisation candidates: superinstructions chosen from profiles (e.g.
`CMP`+`B<c>`, `LDR`+`ADD`), per-register specialised handlers for the hottest
ops, and a direct-mapped page table instead of the hashed block lookup.

## iPhone app

`app/` holds the iPhone app: bundle ID `com.lfull.HydrogenVM`, iPhone only,
iOS 26+, arm64 devices. Generate the Xcode project with XcodeGen
(`cd app && xcodegen`). It has a Library tab (import/delete `.ipa` files;
launching needs roadmap steps 3–5) and a Diagnostics tab (4 GB arena check +
CPU self-test with MIPS). It's intended for sideloading / development builds;
the entitlements below won't pass App Store review for an emulator.

The Swift/Objective-C++ code hasn't been compiled yet (no Xcode in the build
environment); the C++ it calls is the same code the test suite builds.

## iOS-specific notes

* **4 GB reservation**: most iOS devices need the
  `com.apple.developer.kernel.extended-virtual-addressing` entitlement for a
  4 GB `mmap`. Without it the arena constructor throws. A fallback (smaller
  arena plus bounds-masked addressing) is possible but costs an `AND` per access.
* **NULL page**: 32-bit iOS executables start `__TEXT` at `0x1000`, inside the
  first 16 KB host page, so guest NULL dereferences in `0x0000–0x0FFF` read
  zeros instead of faulting.

## Roadmap

1. ~~Integer core with lazy block decoding + tests~~ ✅
2. ~~VFPv3/v4 + NEON~~ ✅
3. **Mach-O loader** (next): 32-bit Mach-O, fat binaries, dyld info binding imports to
   HLE stubs, ObjC metadata.
4. **Guest ObjC runtime + HLE frameworks** (libSystem, Foundation, UIKit basics).
5. **OpenGL ES 1.1/2.0 → Metal** state tracker.
6. Android `.apk` (Phase 3): separate effort — native `armeabi-v7a` libs reuse
   this core, but Dalvik/ART + the Android framework are a project of their own.
