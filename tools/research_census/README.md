# Research census (2026-10-30): reference numbers and fixture manifest

Reference for the import-census numbers in
[`docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md`](../../docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md).
The one-off research scripts are deliberately not committed: they hardcoded workspace paths and had no
tests. Task T4 in [`docs/PLAN.md`](../../docs/PLAN.md) builds the maintained tool (`tools/census.py`)
and must reproduce these numbers.

## Method (what T4 must implement)

1. List imported non-ObjC symbols, referenced ObjC classes and selectors from the slice declared in the
   fixture manifest below (armv6 for Angry Birds, armv7 for the others; the tool must support both and
   select the declared slice) with `llvm-nm` / `llvm-objdump`. Fail loudly if either command exits non-zero.
2. Ask a host program that constructs a `Process` with no app loaded which symbols, classes and
   selectors are registered. Do not regex `src/`: registration is imperative C++ with computed names.
3. Track stub classes (`Runtime::stubClass`) separately from implemented classes.
4. Report unimplemented symbols, missing/stubbed classes and selectors not implemented on any class.

## Fixture manifest (the three IPAs committed in the repo root)

| File | SHA-256 | Slice used |
|---|---|---|
| `AngryBirds_v1.0_os30.ipa` | `cdea1290dcaa67ec75cf555e78caaa8d90cfaa90021ec7f05e40089fb87944cf` | thin armv6 |
| `Art_Of_Glow_v1.1.2_os313.ipa` | `79266fe649b0449aaedefa7228c3d242e2f39b34d8cb2a1f1802467851b96f0e` | armv7 |
| `Tiny_Tower_v1.0_os30.ipa` | `7f84f2d2c561373917a05866337bdf8d8653b3ac8b801248d298f20b5c649578` | armv7 |

## Reference numbers (HEAD `ff235bd`)

| | Angry Birds | Art of Glow | Tiny Tower |
|---|---|---|---|
| Imported non-ObjC symbols | 237 | 435 | 330 |
| Unimplemented symbols | 0 | 12 (data constants) | 0 |
| Missing or stubbed classes | 0 | 2 | 7 |
| Missing selectors + classes + constants | 0 | 88 | 53 |

65% of the 683 distinct symbols across the three apps are used by only one app. A registry of
4,038 symbols (2,888 functions, 1,150 data) and 252 classes was counted by the host program at HEAD.
Raw outputs are not committed. These figures come from one analyst and three apps.
