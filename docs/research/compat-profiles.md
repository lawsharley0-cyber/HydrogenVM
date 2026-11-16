# Compatibility profiles — 2026-11-16

Owner handoff item 4, issue #488. `src/runtime/compat/Profiles.cpp` is the
checked-in table for detection, effects, evidence and verification. It records:

| Profile | Detection | Behavior |
|---|---|---|
| Restored Gameloft content | `Library/Caches/dlcs/mnhtn_index_ios` exists in the current home | Reachability flag 2 while still offline; no socket/DNS permission |
| Groove Zero retired server | Exact normalized `gczero.com` or `staging.gczero.com` | Existing local replies, paks and player-supplied music; no network |
| Allow internet | Explicit current-game `ProcessConfig::internetAccess` | Existing outbound IPv4/TLS policy; off by default |
| Unlock frame rate | Explicit current-game `ProcessConfig::unlockFrameRate` | Existing experimental timer/display-link policy; off by default |

The table preserves **detected conditions**, not an added bundle-title gate.
The restored index is checked on every reachability query just as before;
creating/removing it during a session has the same effect. Stand-in aliases,
URL normalization, development overrides, response bodies and unknown-endpoint
defaulted reporting stay unchanged. Per-game preference keys in the shell stay
`internet.<bundle id>` and `unlockFPS.<bundle id>`. The table describes those
choices; it never creates an automatic internet or FPS override for a title.

Consumers now use the common compatibility accessors. Enabled choices are
announced at Process initialization. Content/server profiles are announced
once when used, **per Process**; a later game gets its own announcement. This
replaces the old process-global reachability `once_flag`, whose log disappeared
on subsequent launches. The ordinary stand-in request logs remain unchanged.
No handler registrations or support labels are changed by this refactor.

Focused tests cover default/explicit choices, live marker predicates, the
offline/network separation, exact host matching, byte-identical alias replies,
unreachable shop behavior, defaulted unknown endpoints and per-process logging.
They are also part of fulltest. Baseline before src edits: Test Lab **41 pass,
0 fail, 0 interrupted**, Linux / WSL2 / 4096-byte pages / baseline `7fdc8a18`.
Remaining game fixtures and physical iPhone behavior require the private runner;
the refactor does not independently qualify them.

After-change local results on `2474be76`: all required targets and the focused
target built; **15 focused checks**, **847 fulltest**, **47 support**, **411
tools tests (4 skips)**, and **41 Test Lab** cases pass. Stub lint reports **0
new/changed** (664 legacy). Baseline Test Lab also passed 41/41 before edits.

Private Angry Birds Island 1.7.0 GPU replays at 45/60 seconds pass crash, stall,
RSS, duration and zero-stub checks. Normalized RGB screenshot MAE against the
pre-refactor captures is **0.000226588 / 0.0000376583** (tolerance 0.04); peak
RSS is 402,915,328 / 404,488,192 bytes. This checks refactor equivalence against
the previous build, not correctness against independently approved references.
Evidence remains private in `HydrogenVM-perf/build/profile-smoke/run-o584i2kf/`.
