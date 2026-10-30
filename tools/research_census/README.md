# Research census (2026-10-30)

Scripts behind the import-census numbers in
[`docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md`](../../docs/research/COMPATIBILITY_STRATEGY_2026-10-30.md).
They were written as one-off research tools: paths inside them point at the research workspace
and must be adjusted before reuse. T4 in `docs/PLAN.md` replaces them with `tools/census.py`.

- `oracle.cpp`: constructs a `Process` with no app loaded and prints which symbols, classes and
  selectors are registered (the regex approach over `src/` is unreliable: registration is imperative C++).
- `census.py`: lists imported non-ObjC symbols, classes and selectors from an armv7 Mach-O slice.
- `implemented.py`, `compare.py`, `truth.py`: compare an app's imports with the oracle output.

Reference numbers (armv7 slices; Angry Birds is thin armv6): imported non-ObjC symbols
237 / 435 / 330 (Angry Birds / Art of Glow / Tiny Tower); unimplemented symbols 0 / 12 data constants / 0;
missing or stubbed classes 0 / 2 / 7; missing selectors+classes+constants (truth) 0 / 88 / 53.
65% of 683 distinct symbols are used by only one of the three apps. Raw outputs are not committed.
Counting rules for the HEAD census (4,038 symbols: 2,888 functions, 1,150 data; 252 classes) are those
of `oracle.cpp` at HEAD `ff235bd`.
