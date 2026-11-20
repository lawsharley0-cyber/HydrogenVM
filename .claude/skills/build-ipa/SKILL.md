---
name: build-ipa
description: Build the HydrogenVM iPhone app (ipa.yml) on main or a branch, wait for it, and produce the artifact link for Lawson. Use after merges or to validate Swift/app changes on a PR branch.
argument-hint: "[branch, default main]"
---

1. Dispatch the build. The ref is "$ARGUMENTS", or `main` if that's empty:
   ```
   R=repos/lawsharley0-cyber/HydrogenVM
   gh api -X POST $R/actions/workflows/ipa.yml/dispatches -f ref=<ref>
   ```
2. Find the run (it appears within a few seconds) and wait for it. It takes about 10–25 minutes; check
   every few minutes in the background, not in a tight loop:
   ```
   gh api "$R/actions/workflows/ipa.yml/runs?per_page=1&branch=<ref>" --jq '.workflow_runs[0]|"\(.id) \(.head_sha[0:7]) \(.status)/\(.conclusion)"'
   ```
3. If it failed, pull the compiler errors:
   ```
   gh api $R/actions/runs/<run>/jobs --jq '.jobs[]|select(.conclusion=="failure")|.id'
   gh api $R/actions/jobs/<job>/logs | grep "error:" | sort -u
   ```
   Common: Swift 6 "sending … risks causing data races". Read the non-Sendable value inside the
   main-actor region instead of capturing it. Fix and push, then dispatch again.
4. On success, get the artifact and build the link:
   ```
   gh api $R/actions/runs/<run>/artifacts --jq '.artifacts[]|"\(.id) \(.name)"'
   ```
   The link is `https://github.com/lawsharley0-cyber/HydrogenVM/actions/runs/<run>/artifacts/<id>`.
5. Give Lawson the link only after success, plus 3–6 plain-language things to test on the phone from
   what merged since the previous IPA.
