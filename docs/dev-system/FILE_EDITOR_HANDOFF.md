# File saving and ordered continuation

Owner request, October 22, 2026: repair the filesystem/save workflow first, then
continue through the handoff. Work is on `codex/file-save-overhaul`, draft PR #18,
based on `main` at `39904cb`. No other checkout's uncommitted changes are included.

## Implemented on this branch

All six editable formats (text, config, JSON, plist, SQLite and binary) use a
common Save action. Each file opens in a sheet with its own NavigationStack.
The sheet owns the persistent Save Changes bar, unsaved-change confirmation,
Revert and optional debounced autosave. Each visible destination supplies its
navigation Save/Close and keyboard Save/Done toolbar. Both use the same session,
so saving and guarded closing remain available throughout nested navigation.
The footer reserves its own layout space and moves with keyboard avoidance;
autosave/settings cannot cover the navigation bar. Text editors use explicit
focus handling for taps in the editing surface. Revert rebuilds the
navigation stack; returning from a nested view no longer reloads the root file.
Changing editor types is disabled until edits are saved or reverted.

`SaveFileStore` retains the loaded disk revision, rejects external modifications
or deleted targets, makes an exact original-byte backup before replacing a file,
writes atomically and reads the result back. Failed serialization, backup,
write or verification retains the dirty editor. Each source has up to ten
revisions under Application Support/HydrogenVM/FileEditBackups; the Backups
sheet exports/restores them. Restoring a revision also backs up the current file.
These are per-file revisions, separate from whole-app snapshots.

Text/config preserve encoding, byte order marks and newline bytes. Config
structured edits preserve unchanged lines, separators, section headers and
comments; values requiring different syntax must be edited in raw mode.
Apple `.strings` uses raw editing to preserve its escaping grammar. Compression
errors never fall back to plain bytes. JSON numbers retain NSNumber precision;
binary keyed-archive UID objects retain their plist type, and keyed archives
retain their original format. Nested compressed plist data is recompressed.
Filtered tree deletion uses selected identities rather than unfiltered offsets.

SQLite edits affect a private working copy until Save. SQLite's backup API
includes WAL content, the source is checkpointed before its revision is loaded,
and pending journal data prevents raw replacement (including Hex mode).
An empty checkpointed WAL or an SHM index alone is not pending database content.
SQL ATTACH/DETACH, including the attach used by VACUUM INTO, is denied in the
editor connection so custom SQL cannot bypass the working-copy boundary.
NULL/BLOB updates report errors; row deletion is transactional.

Snapshot creation stages both save roots before publishing a complete snapshot.
Restore creates a before-restore snapshot, stages every replacement, and retains
the previous roots until installation succeeds. Installation errors roll back;
if rollback fails, the recovery directory is preserved and reported. This is
recoverable folder replacement, not a crash-atomic transaction across two folders.
Imports use a free filename instead of deleting an existing file before copying.
Create/rename accept a single path component.

The old text-pattern keychain synchronization was removed. Editing a selected
file no longer silently rewrites another save/keychain entry or suppresses its
write errors. Edit the actual keychain payload in its plist when that is where
the guest stores its progress. The game must be stopped while editing saves.

## Validation

The focused Apple workflow compiles actual Foundation/Observation persistence
models in Swift 6 and exercises save/reopen, backup retention/failure, external
conflicts, deleted targets, symlinks, empty saves, compression, encodings, config
formatting, exact JSON integers, a real NSKeyedArchiver round trip, SQLite WAL
copy/save/reopen and staged snapshot failure handling. It then builds the actual
iOS shell and runs an isolated SwiftUI harness on an iPhone simulator. The harness
compiles the production editor sources without the emulator or commercial assets.

Final tested source is `d3085f3`:
[Apple validation](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39881742994)
passed the persistence/model tests, all five simulator interaction tests and the
full native shell compile.
[Linux compatibility](https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/39881742977)
passed 64 harness self-tests and all 16 unit/asm/differential configurations,
including 191 runtime checks, with zero baseline/previous-indexed regressions.
The exact report is indexed in STATUS.md. Evidence-only commits do not change
the tested code. Earlier failed UI runs are not treated as passes. Simulator interaction
is separate from signed-device guest behavior. Run these device scenarios before
calling the owner's original device issue resolved:

1. Open an existing text, config, JSON, binary/XML plist and binary/compressed
   save in portrait. Edit, save with keyboard visible, close and reopen.
2. Edit a nested JSON/plist node and compressed Data value; return to the root,
   save, reopen and verify. Decode an edited keyed archive in its real guest.
3. Test Close → Save & Close, Discard Changes and Cancel; verify a failed Save
   keeps the sheet open. Swipe dismissal must be blocked while dirty.
4. Test debounced autosave, Revert from a nested view, format switching after
   saving, and rolling backup export/restore. Check the last typed value survives.
5. Edit SQLite integer/text/NULL/BLOB cells, delete filtered rows and run SQL;
   confirm Discard leaves the original unchanged and Save survives relaunch.
6. Create and restore a full snapshot; verify both Documents and Library,
   preferences, SQLite WAL and the before-restore recovery snapshot.
7. Relaunch the guest in the same sandbox and verify it consumes the edited file.
   A disk round trip alone cannot prove that a game stores progress in that file.

## Ordered continuation

The owner authorized the next player steps after #18. See
[PLAYER_HANDOFF.md](PLAYER_HANDOFF.md) for the stacked Group E continuation,
its exact scope, verification and remaining runtime/device dependencies.

The supplied engineering roadmap is reference material. Its audit statements
were checked against current main/open PRs before selecting work; a listed idea
or historical handoff imperative is not proof that an implementation exists.

| Order | Task | State / dependency |
|---|---|---|
| 1 | UX-1 / B-211 save overhaul | Implemented in draft #18; automated checks pass; review and device scenarios above govern closure |
| 2 | B-003 documentation reconciliation | Updated on this branch: main integration, shell file map, implemented GLES/audio/UIKit, limits and merged-item status |
| 3 | Read-only docs doctor | `python3 tools/hvm_compat.py doctor`; validates commit references in backlog/status/plan. Optional `--report` uses only explicitly backlog-tagged passing cases from a clean ancestor report to suggest status review; never auto-closes tasks |
| 4 | Issue #14 and B-111 graphics limits/point sizes | Wait for Group B PRs #11/#12 and coordinate one owner; no competing edits here |
| 5 | B-109 font-table/query compatibility | Queued; first reproduce the reached calls and add independent expected results |
| 6 | B-101/B-102 guest permissions/precise faults | Queued as one Group A design; needs 16 KiB Apple evidence, not merely a 4 KiB Linux gate |
| 7 | B-106 Clash sector state and progression | Coordinate open #13; use pinned-client decoder evidence and the two-launch probe. No fabricated payloads or progression claims |
| 8 | B-105 raster/compositing, CPU budget/return-stack, B-208 | Profile and compare matched builds after ownership/dependencies clear |
| 9 | UX-2 orientation/viewport/input, UX-3 paused-game HUD | Queued; bridge, host, touch/accelerometer transforms and persistence flushing need end-to-end tests |
| 10 | UX-4 audio lifecycle, UX-5 library/import, UX-6 reports, UX-7 controls | Queued; native lifecycle/input validation required |
| 11 | B-206/B-207 positional printf and stream virtuals | Queued; shared ABI fixtures and compatibility gate required |
| 12 | B-202 libc++ slice | Queued; measured missing imports and ABI coverage before expansion |
| 13 | B-107 scheduled oracle and B-209 sanitizer coverage | Queued; decoder sample size and fault-path constraints remain explicit |

PRs #15–#17 remain separate allocator/PCM/ZIP proposals. This author does not merge
them. Android, FairPlay decryption, cipher/network policy changes and proprietary
fixture removal retain their owner-policy boundaries. None of the queued rows
above is marked completed by this save-editor change.
