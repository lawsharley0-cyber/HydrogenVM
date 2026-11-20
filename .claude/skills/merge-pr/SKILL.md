---
name: merge-pr
description: Verify and merge a HydrogenVM pull request as head engineer (checks, draft state, conflicts, stacked PRs, known CI noise), then close fixed issues. Use when asked to merge a PR or after a review passes.
argument-hint: <PR number>
---

Merge PR $ARGUMENTS in lawsharley0-cyber/HydrogenVM. Lawson delegated merging to the head engineer
(see CLAUDE.md). Use REST through `gh api`, because GraphQL commands such as `gh pr merge` can fail
in cloud sessions.

1. Inspect the PR:
   ```
   R=repos/lawsharley0-cyber/HydrogenVM
   gh api $R/pulls/$ARGUMENTS --jq '{title,draft,state,mergeable_state,base:.base.ref,head:.head.ref,sha:.head.sha,user:.user.login}'
   ```
2. Checks on the head commit:
   ```
   gh api "$R/commits/<sha>/check-runs?per_page=50" --jq '.check_runs[]|"\(.name): \(.status)/\(.conclusion)"' | sort -u
   ```
   - Wait if any check is still in progress, unless it is only the known-broken `editor` check.
   - A red "Linux — tests, report, CoreMark" check is acceptable only when its log shows an apt or
     toolchain download stall *and* another run of the same check on this commit succeeded.
   - Any other red check: read the log
     (`gh api $R/actions/jobs/<id>/logs | grep -E "error|FAIL|Traceback"`), then stop and report or
     fix. Don't merge.
   - App (Swift) changes need a successful `Build unsigned device IPA` check or `ipa.yml` run on the
     branch.
3. The PR body must contain the gate output (fulltest, Test Lab, Android Test Lab, lint, unittest).
   If it doesn't, comment asking for it and stop.
4. Fix the state:
   - Draft: `gh api -X POST $R/pulls/$ARGUMENTS/ccr/ready_for_review`.
   - `dirty`: rebase onto main in a separate worktree, push to a *new* branch, open a replacement PR
     with the same body plus a note, and close this one with a pointer. Never force-push an agent's
     branch.
   - Stacked on another PR branch: merge the stack top-down with `-f merge_method=merge` into each
     parent branch, then squash the bottom PR into main. Retarget the remaining PRs to main
     (`gh api -X PATCH $R/pulls/<n> -f base=main`).
5. Merge: `gh api -X PUT $R/pulls/$ARGUMENTS/merge -f merge_method=squash --jq .message`.
6. For each issue the PR fixes, comment "Fixed in #N (merged); ships in the next IPA" and close it:
   `gh api -X PATCH $R/issues/<i> -f state=closed -f state_reason=completed`.
7. Report in one line what merged, and any noise you merged through.
