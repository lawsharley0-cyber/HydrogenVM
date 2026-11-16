# Player problem reports — 2026-11-16

Owner handoff item 5, issue #490. The expanded player controls and console now
open **Report a problem**, which creates a local snapshot and review sheet.
The snapshot includes game bundle/version, device model/iOS version, HydrogenVM
version/build/commit, the sanitized buffered run diagnostics and sanitized
machine crash JSON when available. Player crash sheets use the same report.

Nothing is sent or copied on construction or when the sheet opens. The user
explicitly chooses Copy report (complete JSON), Share report (system share
sheet), or Open GitHub draft (metadata-only editable issue). The GitHub URL
never carries the diagnostic log/crash contents. The user attaches or pastes
the reviewed report and submits the issue themselves.

Raw guest output cannot be assumed free of account data. The export therefore
constructs a diagnostic log from recognized categories, machine addresses and
runtime symbol forms; it omits free-form lines, account/credential-related
lines, URLs, quoted values and **all paths**, including sandbox paths. Omitted
line counts are explicit. It does not export HTTP bodies, preferences, saves,
device identifiers or arbitrary crash dictionaries. Crash export keeps only
numeric machine fields/registers, a hex signature and filtered backtrace
symbols. This is a sanitized diagnostic subset, not an unmodified raw crash
dump or a complete guest NSLog transcript. The original private crash journal
is preserved locally for owner-directed investigation.

The pure-Foundation Swift regression file exercises metadata, numeric crash
retention, credential/email/path/URL omission, malformed crash JSON, bounds,
absence of crash fields for a normal run and metadata-only issue URLs. It is
wired into `file-editor.yml` before native shell compilation.

Initial local validation: tree-sitter Swift syntax parsing passes for all four changed
Swift files; workflow YAML parses; `git diff --check` passes. This Windows/WSL
host has no Swift compiler or Xcode, so the Swift tests, Apple compilation,
share-sheet interaction and physical iPhone flow are **NOT RUN locally**.
Keep draft until native CI and the owner's phone checks pass. `ipa.yml` is
manual-only; the connected GitHub tool has no initial dispatch operation and
local gh is unauthenticated. A `[mac]` PR title also requests native CI.

Subsequent native evidence at production source `3f1c836f`: Swift 6 report
privacy/metadata tests and unsigned simulator compilation passed. The full UI
phase stopped before running tests because the existing engine-free HVMSession
fixture lacked `playSession`, `allowInternet` and `unlockFrameRate`, already
used by RunModel. The fixture now mirrors those three stored Bool properties;
no production behavior or test assertion is changed. Report interaction remains
unqualified until the corrected fixture's UI run completes.

The requested `ipa.yml` was dispatched through the authenticated browser:
[run #54](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/43914721340).
Its first attempt failed before starting due to hosted macOS arm64 capacity
and a GitHub internal server error. One connector retry was accepted and is
building the device package at the latest observation. No successful package
or physical phone flow is claimed yet.
