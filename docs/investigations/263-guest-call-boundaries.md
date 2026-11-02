# #263/#125: guest-call argument writes can escape the fault scope

`Engine::callGuest` writes stack arguments before `Engine::run` installs its
guest fault scope. On main `4c09fa47`, the subprocess regression demonstrates:

```
ok valid cross-page arguments
FAIL unmapped stack escaped as host signal 11
FAIL partial cross-page write escaped as host signal 11
FAIL read-only stack
FAIL stack underflow escaped as host signal 11
FAIL argument count overflow escaped as host signal 11
```

After the fix all six checks pass. A complete-range write check holds the arena
mapping mutex across validation and copy. It reports the first non-writable
guest byte, rejects arithmetic overflow/underflow before touching host memory,
and preserves caller registers, call-boundary state, earlier bytes and the
first guest fault. It neither maps missing pages nor grants extra permissions.
The cross-page canary and read-only checks use independent requested addresses
and known bytes, rather than copying emulator output into an expectation.

This confirms a shared host-fault class, not the cause of Goat's historical
219-second crash. No occlusion-query or game-specific shortcut is added; the
unmerged G3 branch and other agents' branches remain untouched. Final fulltest,
Test Lab, tools and ASan/UBSan results belong in the PR before readiness.

## Private long-session observation

The available Goat IPA has SHA-256
`4ba17aa9b773179823e18765552ff8252d2cfee096645c38a4db4e1e19f3bed1`,
version 1.0.3, build 7685.1601 and the unusual identifier
`com.coffeestainstudios.goatsimulmodifed ` (including trailing space). This is
not proof of an unmodified retail client.

A software-rendered 420-second probe used the #283 diagnostic candidate source
`ccc309fc84c5baf3428b2f650ec235a04e764a2e`, executable SHA-256
`e107215fd8a47e80be39b4e6a21ba602df485133b471a617660dc84808ce5df9`.
It queued Play at portrait logical coordinates (260,90) at 30 seconds and a
later recorded drag. A live capture showed the menu removed and a changing 3D
scene under a persistent logo. Full world/control qualification is unverified.
The probe did not observe a host SIGSEGV. It ended at 425.36 seconds with a
graceful-stop watchdog failure: `crashed=true` despite the old CLI reporting
status zero. That inconsistency is addressed separately under #234, not counted
as a successful quit or a resolved long-session crash.

The private report/log/input/frame files remain below HVM_FIXTURE_ROOT;
the session log SHA-256 is
`55e0930268ded2e359257deaa7184be4517b8d5bc578dbfbe13a9a445069798e`.
No real IPA, save, screenshot or raw game log is committed.

## Device capture still required

Use the matching signed HydrogenVM build and record its full commit, device/iOS
and native page size, private IPA hash/version, preserved home state and exact
world-entry inputs. Capture at least five minutes *after confirmed world
entry*, including control responses. At the first fault export the recovered
#264 JSON/host-fault text, complete session log, iOS .ips report and matching
dSYM/UUID. Record faulting host address, arena offset and crashed thread. A
Linux non-reproduction or a synthetic fault address does not identify the
historical device fault. Keep #125/#263 open until matching evidence exists.

Final local candidate gates (Linux native pageSize 4096): fulltest 488/488; Test Lab 25/25, zero skips; enabled tools 202/202, zero skips. Focused ASan/UBSan passes all six cases. These results precede the documentation-only commit; the published source/tree fingerprints and Actions runs are recorded on the PR.
