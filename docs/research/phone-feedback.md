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

Local validation: tree-sitter Swift syntax parsing passes for all four changed
Swift files; workflow YAML parses; `git diff --check` passes. This Windows/WSL
host has no Swift compiler or Xcode, so the Swift tests, Apple compilation,
share-sheet interaction and physical iPhone flow are **NOT RUN locally**.
Keep draft until native CI and the owner's phone checks pass. `ipa.yml` is
manual-only; the connected GitHub tool has no dispatch operation and local gh
is unauthenticated. A `[mac]` PR title also requests the existing CI native
build, but does not replace the requested manual `ipa.yml` qualification.
