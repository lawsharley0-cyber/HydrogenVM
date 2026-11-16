# Scripted smoke harness qualification — 2026-11-16

Item 1 of the owner's Codex handoff, issue #486. Runtime baseline:
`7fdc8a18aad97ee13ae8f3df8d272e4235880ba8`, Linux x86_64 under WSL2,
Clang 18.1.3, Release, 4096-byte host pages. No runtime source changes.

Synthetic coverage: 12 tests exercise crash/stall/load/exit/early-finish,
required-log, missing/corrupt-identity fixture behavior, RSS limits and missing
measurement, timeout/kill, path containment, isolated fresh homes, reference
requirements, tolerant image comparison and CLI skip summaries. Stub audit:
664 legacy findings, **0 new/changed**. Fulltest: **846 passed / 0 failed**;
support: **47 passed / 0 failed**. All three required targets built locally.
Tools suite: **423 tests, OK, 4 skipped** after correcting checkout-only CRLF
shell scripts and running without a concurrent fulltest (the movie check uses
shared temporary files). Baseline Test Lab: **41 passed / 0 failed**.

Available private fixture: Angry Birds Hatchery Island HD 1.7.0, bundle
`com.rovio.angrybirdsislandhd`, extracted unchanged under the owner's bench.
Its documented fixture SHA is
`64fea529818bffd7596a696b4bb22e4f32e1843c24b8b892ee1eb5c1154ccac3`;
this run used the existing extracted app and did not re-hash an IPA archive.
A private copy of the checked-in scenario changed only `ipa` to the extracted
relative app path. GPU mode, network off, fresh home per replay, documented
20/26/32/42-second menu taps:

| Endpoint | Result | Sampled peak RSS | Stub hits | Screenshot |
|---|---|---|---|---|
| 45 s | process checks PASS | 404,901,888 bytes | 0 | 768×1024 produced; comparison SKIPPED |
| 60 s | process checks PASS | 400,773,120 bytes | 0 | 768×1024 produced; comparison SKIPPED |

Both exited 0 after the requested duration without a crash or stall report.
Private evidence is in `build/private-smoke/run-bmvmiu9y/`, excluded from Git.
No approved image references were supplied; no visual correctness or full
gameplay qualification is claimed. The harness did not create reference images.

Minion Rush, Groove Coaster Zero and Army of Darkness fixtures were not present
in this bench directory, so their scenarios were not executed. Their RSS
ceilings and noted tap timings need owner/runner qualification. Missing games
and missing visual comparisons remain explicit skips. Full scenario commands
and private CI configuration are in [the testing guide](../testing/smoke.md).

## Expanded fixture search

A later search outside the primary bench found the owner's Army of Darkness
1.0.1 archive. Its 90-second script reaches the first movement tutorial, but
**FAILS** at graceful shutdown: exit 70, `graceful-stop-watchdog`, signature
`d5c54ff6d981`. The same failure reproduces on unchanged runtime `7fdc8a18`
(frozen build `6993ed89`, tests-only additions) and on profile refactor
`2474be76`; it is not newly attributed to the refactor. Candidate peak RSS
510,590,976 bytes, no stub hits. The harness correctly retains the failure.
No shutdown check or test is waived. Follow-up issue: **#496**.

An older local Minion Rush 1.0.0 archive has no restored DLC index, so it is
not the requested with-data fixture. The manifest now requires nonempty
`Container/Library/Caches/dlcs/mnhtn_index_ios`; both synthetic missing/empty
archives and the actual no-data archive are rejected before launch. Focused
harness tests are now **13 passed**. Groove Coaster Zero and a matching
with-data Minion Rush fixture remain unavailable. Initial skip observations
above describe the original primary-bench search, not this later result.

## Owner-supplied fixtures: November 16 follow-up

The supplied IPA folder resolved the two missing fixtures. Original archives were
left unchanged; private scenario copies override only the IPA filename.

| Fixture | Archive SHA-256 | Duration | Result | Peak RSS |
|---|---|---:|---|---:|
| Minion Rush 1.0.0 with data | 3d162e5ebeb1044ed04cac55f27a9e0a07f3515263c5fc8d3603d3ba7f048c64 | 300 s | PASS, exit 0 | 1,018,109,952 bytes |
| Groove Coaster Zero 1.0.0 | 0d5bc908afe2a432dabe098f72483083421a06416dd3ca5e73b19c3abf1fd228 | 60 s | PASS, exit 0 | 234,594,304 bytes |

Both fresh-home GPU runs used profile-refactor runtime 2474be76, internet/FPS
overrides off, with zero crash/stall/stub reports. Minion's restored index is
18 bytes with seven data packs. Groove's required stage.pak request was logged.
Screenshots show Groove's title screen and Minion's introductory notice; the
historical Minion tap did not dismiss that notice. No terms were accepted and
no level-entry/gameplay claim is made. A Minion thread-exit autorelease warning
remains in its private log; the process nevertheless exited 0 under unchanged
criteria. Approved image comparisons remain SKIPPED.

Private evidence: build/found-fixture-smoke/run-idlrywy_/. Army #496 remains a
failure. Earlier unavailable statements record the initial search only.
