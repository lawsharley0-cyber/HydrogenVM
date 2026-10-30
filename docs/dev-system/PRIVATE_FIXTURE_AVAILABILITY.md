# Trusted private fixture availability diagnostic

Issue #204 separates retrieval/integrity from real-app runtime parity. The
`Private fixture availability (trusted main)` workflow checks the former on
Linux and macOS. It does not replace required compatibility, GPU or simulator
coverage, change a baseline, or authorize removal of the three root IPAs.

## Existing configuration required

Recover the resources already provisioned for project `hydrogenvm-fixtures`:

- `HVM_WIF_PROVIDER`: the complete existing provider resource:
  `projects/PROJECT_NUMBER/locations/global/workloadIdentityPools/POOL/providers/PROVIDER`.
- `HVM_FIXTURE_SERVICE_ACCOUNT`: the existing fixture-reader service-account email.
- `HVM_DRIVE_FOLDER_ID`: the restricted folder containing the three original named IPAs.
  Store the folder ID only as a repository secret.

The manual dispatch accepts no credential, resource or ref inputs. The folder ID
is consumed only from the existing repository secret and masked in logs. Provider
and account resource strings use the two repository variables above. No credential
is stored by this workflow and no IAM/API/Drive permission is changed.

The existing WIF provider must already restrict repository and owner identity,
`refs/heads/main`, and trusted event names; the job itself admits only manual
dispatch on canonical main. Existing service-account impersonation must permit
the dedicated reader, and that account must already have Viewer access to the
restricted Drive folder. Failed authentication or missing files fail the job.
Do not broaden IAM or make the folder public to bypass a failure.

## Run and interpret

After independent review and merge, select this workflow on `main` and dispatch
it with existing configuration. Branch/fork dispatches cannot run the job or
request its OIDC token. It checks out the dispatch SHA, not an input ref.
The workflow requests a ten-minute Drive read-only OAuth token through the
existing keyless identity, without a credentials file or exported ambient SDK
credentials. Action dependencies are pinned by commit.

Both hosted platforms must stage all three files using the existing fail-closed
downloader and independently resolve their manifest SHA-256 values with source
`fixture-root`, with legacy fallback disabled. Missing, mismatched or skipped
required fixtures fail. An optional Clash fixture is outside this three-file
availability gate and is not represented as passed.

Private bytes stay outside checkout under the runner temporary directory and
are deleted in an always-run cleanup step. No fixture, credential, archive,
screenshot or downloaded game content is cached or uploaded as an artifact.
Only counts, the checked-out source SHA and explicit coverage limits appear in
the step summary. Runner destruction is the final cleanup boundary if a job is
forcibly terminated before its cleanup step.

A successful run proves access and byte identity on those runner platforms.
It does not execute games or establish compatibility/frame/crash parity.
Before any removal, separately migrate existing consumers without reducing
their assertions, preserve the real-app baseline, and demonstrate required
Linux compatibility/GPU and macOS simulator scenarios against these private
fixtures. Preserve historical reports and label every unrun scenario NOT RUN.

Validation of this new workflow: NOT RUN until trusted-main dispatch is possible.
Existing synthetic tests run in the diagnostic before authentication. No local
tests/builds or private retrieval were performed while preparing this change.
