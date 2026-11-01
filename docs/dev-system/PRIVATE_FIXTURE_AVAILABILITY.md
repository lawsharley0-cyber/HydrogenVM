# Trusted private fixture pre-removal parity gate

Issue #204 requires real-app parity before removing root IPAs.
`.github/workflows/private-fixture-availability.yml` is a manual trusted-main
gate using the existing restricted Drive source. No existing CI consumer,
baseline, test assertion, or IPA is changed by this infrastructure PR.

## Existing configuration

- Repository variable `HVM_WIF_PROVIDER`: existing complete WIF provider resource.
- Repository variable `HVM_FIXTURE_SERVICE_ACCOUNT`: existing dedicated reader email.
- Repository secret `HVM_DRIVE_FOLDER_ID`: existing restricted folder containing
  the three original named fixtures. Do not publish it as a workflow input.

There are no dispatch inputs, JSON keys, stored credentials, or IAM changes.
The existing provider must independently enforce canonical repository/owner,
`refs/heads/main`, and trusted event names. The workflow admits only manual
dispatch on canonical main. These are defense-in-depth guards: someone who
edits a branch workflow can remove its YAML guard and request GitHub OIDC,
but the external WIF policy must reject exchange for that branch/ref/event.
Do not broaden IAM, impersonation, or folder sharing to bypass a failure.

The dedicated reader already needs Viewer access to the restricted folder.
Auth uses the official commit-pinned Google action, Drive read-only scope,
a ten-minute OAuth token, no credentials file and no ambient SDK export.
Only staging receives the access token as a step-scoped environment value.

## Required scenarios

After independent review and merge, dispatch on `main`. Both jobs check out
the same dispatch SHA and stage/hash-check all three fixtures. The resolver
must report `fixture-root`; legacy fallback is disabled explicitly.

Linux must build the engine and CPU guests, pass heap regressions, run full
compatibility including `fulltest` against the unchanged baseline using
`--no-legacy-repo`, and pass every current Test Lab case with the existing
ARMv7 guest linker. Both existing GPU/software comparison modes run all three
private fixtures. A missing GPU backend or skipped comparison fails this gate,
including the existing comparison tool's otherwise successful skip behavior.

macOS depends on Linux. It builds the production unsigned device and simulator
targets using the existing CI commands. The only transferred executable is the
repository's synthetic UIKitTest guest, built by its existing packaging helper.
Required simulator scenarios preserve current CI inputs and markers:
UIKit `ALL CHECKS PASSED`; Tiny Tower GPU frame 60 with its tap; Angry Birds
landscape GPU frame 600; Tiny Tower GLES1-on-ES2 frame 60. None is
`continue-on-error`. Each scenario also requires its captured PNG screenshot.

Only bounded parity JSON is retained: source commit, page size, fixed scenario
outcomes, required fixture hashes/source, frame counts/classifications,
Test Lab count, and simulator screenshot sizes/hashes. No private IPAs,
extracted assets, screenshots, raw guest logs, credentials or whole run
directories are uploaded/cached. The synthetic UIKit artifact is separately
whitelisted and retained for one day. Private work directories are under the
temporary fixture root and removed in always-run cleanup; simulator guest
copies are uninstalled. Forced termination relies on hosted-runner destruction.

A failed/skipped prerequisite or missing evidence produces NOT VERIFIED and
fails the job. A Linux failure skips the dependent macOS job, so total parity
is not established. Optional Clash absence is not represented as passed.
No physical iPhone qualification is claimed. Before deletion, inspect both
platform summaries and original Actions outcomes, then migrate all existing
consumers while preserving coverage and historical evidence. Removal remains
a separate change after parity and the owner's authorization.

Validation of this revision: NOT RUN. No local tests/builds or private retrieval
were performed. New synthetic evidence-boundary tests and existing retrieval
tests execute in Actions before authentication. Prior parser failure and its
runner-environment fix are preserved in commit history; that fix alone does
not prove this expanded workflow passes.
