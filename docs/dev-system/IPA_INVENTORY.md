# Tracked IPA inventory (issue #204)

Three real-game IPAs are tracked in the repository root (`git ls-files '*.ipa'`).
They are grandfathered by `AGENTS.md`; deleting them is an owner decision. This
file records what depends on them so a later removal change has a checklist.
It changes no consumer, baseline, or workflow.

| File | Bytes | SHA-256 |
|---|---:|---|
| `AngryBirds_v1.0_os30.ipa` | 9,769,211 | `cdea1290dcaa67ec75cf555e78caaa8d90cfaa90021ec7f05e40089fb87944cf` |
| `Art_Of_Glow_v1.1.2_os313.ipa` | 10,433,994 | `79266fe649b0449aaedefa7228c3d242e2f39b34d8cb2a1f1802467851b96f0e` |
| `Tiny_Tower_v1.0_os30.ipa` | 21,483,310 | `7f84f2d2c561373917a05866337bdf8d8653b3ac8b801248d298f20b5c649578` |

Total 41,686,515 bytes. The hashes match `tools/compat_harness/manifests/apps.json`,
`docs/dev-system/library.json` and `tools/census.py`. All three apps are
`optional: false` in `apps.json`, so a missing fixture fails the compat run.

## Consumers of the repo-root copies

| Consumer | What it needs the game for | Class |
|---|---|---|
| `compat-harness.yml`: `hvm_compat.py run --allow-legacy-repo` | Boots each app and diffs against `baselines/linux-headless.json` | Real game |
| `compat-harness.yml`: two `gpu_compare.py` steps | Software vs GPU framebuffer of each game | Real game |
| `ci.yml` macOS simulator: Tiny Tower and Angry Birds GL steps | GPU frame markers (`continue-on-error`) | Real game |
| `tools/census.py --all` and `test_t4_support.TestCensus.test_reproduces_reference_numbers` | Reference counts of unimplemented symbols in the real binaries | Real game |
| `test_manifest.test_legacy_hash_matches_grandfathered_file` | That the grandfathered file hashes to the manifest | Resolver mechanics (synthetic twin: `test_legacy_resolution_with_generated_file`) |

Not affected by deleting from the current tree:

- `clash-royale.yml` fetches the three files from pinned commit `ceba289`. Rewriting
  history would break it.
- `tools/local_test.sh regress` reads `HVM_LOCAL_FIXTURES`.
- `private-fixture-availability.yml` reads the private store.

The Python suites were run with and without the three files (build present). Only
the census test and the legacy-hash test change behavior, and both **skip silently**
when the files are absent. A skip is not a pass.

## Reading the real games from the store

`tools/census.py --all` and the census test now take `HVM_FIXTURE_ROOT` (or
`--fixture-root`) and read `<root>/ipas/<name>`. With a root set the repo copy is
never used, the same rule as the compat harness. `census_ipa` still refuses a file
whose SHA-256 differs from the table above.

## Not yet established

- The pre-removal workflow `private-fixture-availability.yml` has not completed
  successfully (see `PRIVATE_FIXTURE_AVAILABILITY.md`). Linux and macOS retrieval
  from the owner's store is unverified.
- `pull_request` runs do not get store credentials, by design. After removal the
  three required apps cannot run on pull requests unless the owner chooses a
  policy (trusted-event coverage only, or a different gate). That decision, and any
  baseline change, belongs to the owner.
- Deleting the files from the current tree does not remove them from history.
