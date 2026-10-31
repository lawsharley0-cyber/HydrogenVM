# T8: Objective-C conformance cases — approved design

Owner: **Codex** · Card: [#241](https://github.com/lawsharley0-cyber/HydrogenVM/issues/241) · Status: **approved and implemented; local gates passed**.

**Goal.** Add five ARMv7 Test Lab cases covering public runtime behavior from
objc4's tests. Proposed approach: project-authored behavioral ports of selected
assertions, not a build of Apple's modern test harness. Lawson approved this
scope and authorized implementation after reviewing draft PR #247. No
Apple-source import or dependency completion is approved by that
review. T1/T2/T3 remain the recorded integration prerequisites.

**Reference and licence boundary.** Pin Apple's
[objc4 revision fb265098](https://github.com/apple-oss-distributions/objc4/tree/fb265098298302243cd7eeaa1f63f0ba7786dd9a).
Use its [public API contracts](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/runtime/runtime.h)
and the test topics below as references. Do not copy source, assertions, headers,
macros or helpers into HydrogenVM. Before any source adaptation or vendoring,
Lawson must record the file scope and licence decision on #241, considering the
repository's [APPLE_LICENSE](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/APPLE_LICENSE)
and applicable file notices. This design does not resolve licensing or authorize
Apple-source imports. CoreFoundation algorithms are outside this card.

**Cases and independent oracles.** IDs TL-S–TL-W were free on integration main
when implementation began. Each fixture records its reference, contract derivation,
expected values and raw observations. Expected values never come from HVM output.

| ID / upstream topic | Fixture and expected result |
| --- | --- |
| TL-S / [sel.m](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/test/sel.m) | Register the same name from distinct buffers; both SELs and the compiled selector agree. A different name differs; `sel_getName` round-trips the bytes. Oracle: selector identity/name contract. Exclude private lookup APIs and null-name conventions. |
| TL-T / [super.m](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/test/super.m) | Declare Base : NSObject and Child : Base. Child's superclass is Base; Child's metaclass superclass is Base's metaclass. Class/metaclass flags are false/true. Oracle: declared inheritance graph; exclude private class layouts. |
| TL-U / [getMethod.m](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/test/getMethod.m) | Base instance method returns 13, Child override 23, inherited class method 31; typed IMP calls produce 13/23/31. Missing methods return NULL. Oracle: authored method constants and public lookup rules; no absolute addresses or private forwarding entry points. |
| TL-V / [addMethod.m](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/test/addMethod.m) | Warm dispatch twice. Add Child override (29) over inherited Base value (11): YES; duplicate add: NO. Replace it with 47: prior IMP returned; Child now 47, Base still 11. Repeat dispatch after each mutation. Oracle: hand-derived mutation sequence and known IMP identities. |
| TL-W / [association.m](https://github.com/apple-oss-distributions/objc4/blob/fb265098298302243cd7eeaa1f63f0ba7786dd9a/test/association.m) | Retain a counted value through a non-atomic association; release caller ownership. Verify identity and drain getter autoreleases before clearing with nil. Dealloc count: 0 before clear, 1 after clear and owner release. Oracle: explicit ownership ledger; exclude private policies, isa changes and races. |

**Integration.** Put fixtures in new `tests/ios/apps/testlab/objc_conformance.m`
and `objc_lifetime_mrc.m`; expose plain C diagnostics to `main.m`'s TLResult,
checkpoint, automated dispatch and phone menu. The existing builder recognizes
`_mrc.m`; add both sources in `tools/hvm_testlab.py` without changing main's ARC
mode. Review any needed project-authored public declarations in
`tests/ios/include/objc/`. Update CASE_IDS, runner tests and `docs/TEST_LAB.md`.
The current schema accepts string IDs; add validation coverage, not a new schema
version or weaker requirements. Isolate every automated case in its own process.

**Acceptance and risks.** Run the new cases against unfixed main first. A passing
case adds coverage; it does not prove a repair. Any proposed runtime fix needs a
recorded pre-fix failure, the same case green afterwards, and ownership clearance
before hotspot edits. Run all Test Lab cases, fulltest, heap and compatibility
gates; retain JSON with build/fixture hashes and host.pageSize. Native Apple
execution is a useful independent cross-check when available; record NOT RUN
otherwise. No claim covers private APIs, pointer authentication, Swift/GC, full
upstream test parity, gameplay or 16 KB devices from a Linux pass. Cap: eight
edit/build/test cycles. The design review is approved; do not infer a licence
decision or prerequisite completion from that approval.
