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
