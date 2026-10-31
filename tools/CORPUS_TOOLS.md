# Corpus bisect and A/B comparison

Both tools run on Linux/macOS and write private evidence under an ignored build
directory. They do not modify production sources or proprietary guest assets.

## Find the first regression

```sh
python3 tools/bisect_corpus.py --repo . --good GOOD_SHA --bad BAD_SHA \
  --baseline docs/dev-system/baselines/linux-headless.json \
  --out build/bisect-result
```

This creates a detached disposable worktree. Each gate uses a fresh build/output
directory, builds the CLI/test targets and ARM guests, runs the compatibility
harness, validates its JSON, and compares against the supplied baseline. It
checks both endpoints before invoking `git bisect run`. It preserves the caller's
branch, dirty files, index and any existing bisect session. The temporary worktree
is removed afterwards. Reports, logs and diffs survive in `--out/<commit>/`;
`result.json` names the first bad commit. A repeated test of one commit replaces
that commit's evidence. The baseline is not updated.

For an existing build pipeline or synthetic corpus, replace the pipeline with:

```sh
python3 tools/bisect_corpus.py --good GOOD --bad BAD --baseline baseline.json \
  --out build/bisect-result --command python3 /absolute/path/gate.py '{repo}' '{report}'
```

`--command` consumes all remaining arguments, runs without a shell in the
candidate checkout, and must write a full schema-valid `hvm-compat-report` to
`{report}`. Supported substitutions are `{repo}`, `{build}`, `{out}`, `{report}`;
other braces are literal. Use absolute paths for external runners. Exit 0/1 is
accepted only with a report. Nonzero preparation, exit codes other than 0/1,
timeouts, missing/invalid reports, unresolved fixtures, omitted baseline cases or
apps, and mismatched host page sizes abort the search. They never become a bad
commit. A failed summary or a baseline regression marks a measured commit bad.
A passing report with exit 1 is a contradictory infrastructure result.

Return codes: **0** unique culprit found; **2** invalid endpoints, unavailable
gate or failed/ambiguous search. The internal gate uses **128** to make Git abort.
Compatibility regressions must be monotonic along the searched history; flaky
results and multiple independently introduced/fixed regressions need a narrower
range. Keep all fixtures local and available across the entire range.

## Compare two builds with the same input

```sh
python3 tools/ab_diff.py /path/old-build /path/new-build --guest /private/Game.app \
  --seconds 10 --timeout 30 --tap '160,240@1' --drag '20,30,200,300@2' \
  --lifecycle 'bg@4,fg@6' --out build/game-ab
```

A build argument is a directory containing `hvm` or the executable itself.
Each side gets a fresh home and exactly the same target, duration, guest args,
taps, drags and lifecycle events. Output directories must not exist; this avoids
reusing saves or stale reports. `command.json`, raw reports, separate stdout and
stderr logs, final PNG and latest sampled RGBA frame are retained on each side.
`diff.json` records the first differing JSON path in each observation category:
report, logs, frame hashes and audio counters, plus process exit status. Its
final `summary` object records equality. It also retains raw performance data.

Normalization excludes performance timing/throughput fields (keeps frame
counts), replaces isolated host paths and the derived guest sandbox UUID, and
leaves other content intact. Audio counts and frame counts/hashes are compared
exactly. Explicit `--ignore-log-regex` options can exclude known nondeterministic
log lines; every expression is recorded in the output. Avoid broad patterns that
hide errors. There is no general numeric/address scrubbing.

This is **not semantic replay**: host-scheduled timers, threads, RNG, network and
audio callback timing can still diverge, including between identical builds.
Use a deterministic guest/scene and inspect each reported difference. Final and
latest sampled frames do not locate an earlier transient rendering divergence.
A timed run's equality is not evidence that gameplay works. Missing/malformed
reports, missing executables and timeouts return **2**; measured
guest crashes can be compared. Return **0** means identical observations and **1**
means a measured divergence, which is not automatically a regression. A load
failure with a valid report is a measured observation too. Binary SHA-256, host
page size and an IPA SHA-256 (when the target is a file) are recorded; a directory
target is identified by its path rather than a full resource-tree hash.

## Validation

```sh
python3 -m unittest discover -s tools/tests -p test_corpus_tools.py -v
python3 tools/tests/validate_corpus_live.py --build build \
  --guest build/ios/TestLab.app --out build/corpus-live
```

The unit suite constructs a ten-commit Git repository, plants the first bad
commit at index five, and checks the exact result and preservation of dirty
caller state. It also checks a missing runner, missing report, mid-search
infrastructure error, script equality, home isolation and every A/B category.
The existing `hvm_report.py` CPU gate runs this suite in CI and records tool
failures in its structured report and overall exit status.

The live validation uses Ninja's actual compile/link commands to create a
separate deliberately broken executable. Only a generated copy of `Bundle.cpp`
is modified: `NSBundle infoDictionary` returns nil for every app. It recompiles
that object, replaces it in a **copy** of the static library and links a separate
CLI. The original build, production sources and Test Lab app are untouched.
TL-A must pass on the original executable and fail on the planted one. A/A must
report IDENTICAL and A/B DIFFERENT. This is an intentionally invalid test artifact,
never a proposed runtime change. Inspect `validation.json` and both guest logs.
