#!/usr/bin/env bash
# CI only: commit a report directory to the `ci-reports` branch so results can
# be read with plain git (job logs and artifacts aren't always reachable).
#   tools/publish_report.sh <report-dir> <job-name>
# Layout: latest/<job>/...  and  runs/<date>-<run id>-<job>/...
set -uo pipefail
SRC=$(cd "$1" && pwd) JOB=$2
BRANCH=ci-reports
STAMP="$(date -u +%Y%m%d-%H%M%S)-${GITHUB_RUN_ID:-local}-$JOB"
WT=$(mktemp -d)
git config --global user.name "HydrogenVM CI"
git config --global user.email "ci@users.noreply.github.com"

for attempt in 1 2 3 4 5; do
  rm -rf "$WT"; mkdir -p "$WT"
  if git fetch -q origin "$BRANCH" 2>/dev/null; then
    git worktree add -q -f "$WT" "origin/$BRANCH" && (cd "$WT" && git checkout -q -B "$BRANCH")
  else
    git worktree add -q -f --detach "$WT" && (cd "$WT" && git checkout -q --orphan "$BRANCH" && git rm -rqf . >/dev/null 2>&1 || true)
  fi
  mkdir -p "$WT/runs/$STAMP" "$WT/latest"
  cp -R "$SRC/." "$WT/runs/$STAMP/"
  rm -rf "$WT/latest/$JOB"; cp -R "$SRC" "$WT/latest/$JOB"
  # keep the 200 newest runs
  n=$(ls -1d "$WT"/runs/* 2>/dev/null | wc -l)
  if [ "$n" -gt 200 ]; then ls -1d "$WT"/runs/* | sort | head -n $((n - 200)) | while read -r d; do rm -rf "$d"; done; fi
  (cd "$WT" && git add -A && git commit -qm "report $STAMP (${GITHUB_SHA:-local})" && git push -q origin "$BRANCH") && { echo "published runs/$STAMP"; git worktree remove -f "$WT"; exit 0; }
  git worktree remove -f "$WT" 2>/dev/null
  sleep $((attempt * 3))
done
echo "could not publish report" >&2
exit 1
