# Support declarations and the stub audit

`src/runtime/Support.hpp` adds explicit metadata without changing guest behavior.
Use `supportDef<&handler>(process, symbol, SupportStatus::Stub, "one-line reason")`
instead of `process.def`, or `supportMethod(process, builder, className, selector,
lambda, status, provenance, types[, classMethod])` for an Objective-C method.

Statuses describe the registered implementation:

| Status | Meaning |
| --- | --- |
| Real | Implements the API contract; provenance identifies the implementation/oracle |
| Approx | Implements a documented subset or approximation |
| Stub | Placeholder, fabricated result, or ignored operation |
| Unavailable | Explicit unsupported-operation implementation |

`supportRegistry(process).snapshot()` returns a sorted, thread-safe copy of the
explicit declarations, suitable for T4's support manifest. `find(name)` returns
no record for an unclassified registration. **Missing metadata never means
Real.** Names are exported C symbols or `-[Class selector]` / `+[Class selector]`.
The registry is owned by the process and is not shared between app launches.
Re-registering a declaration replaces its metadata. The HostGL chain updates
labelled software entries to Approx because driver dispatch can still fall back
to the software implementation or lack a driver function.

The first migration labels the cases named in T3. It does not claim that all
other registrations are implemented. New registration backends and overrides
must update metadata too; existing unlabelled APIs remain audit work.

Run the audit with:

```sh
python3 tools/lint_stubs.py
python3 tools/lint_stubs.py --baseline tools/stub_lint_baseline.json --json build/stub-audit.json
python3 -m unittest discover -s tools/tests -p test_lint_stubs.py -v
build/hvm_support_tests
```

The first command exits 1 if there are any findings. CI uses the second command:
it **prints every finding**, including legacy debt, and exits 1 for a new or
changed unlabelled body. The committed baseline records candidates for review,
not declarations of correctness and not exemptions from the report. Each key
includes the source path, registration, handler, reason and tokenized body hash;
changing an acknowledged implementation reopens the finding. Line changes alone
do not. Do not regenerate the baseline to accept new stubs. Label the registration
with an accurate status and provenance instead. Removed findings need not be kept
in the baseline; baseline cleanup is safe.

This dependency-free lexical lint follows direct `.def`, `.m`, `.cm`,
`.method`, `.classMethod`, `.defineFunction` and `.registerHostFunction` calls
to same-file functions or direct lambdas. It detects constant 0/1/true/YES returns,
constant result-register assignments, empty bodies and fabricated output 1.
Comments and strings do not become code; nested scopes are balanced. Results
include legitimate constant-return implementations, so a finding requires human
classification. Macros, generated registrations, cross-file bodies, indirect
helpers and semantically equivalent expressions are outside its coverage. A
clean lint result is not evidence of runtime compatibility. Status accuracy is
reviewed against the implementation and tests.
