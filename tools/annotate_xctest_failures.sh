#!/usr/bin/env bash
# Turn failing XCTest assertions in an xcodebuild log into workflow annotations,
# so a failure is readable on the run page without downloading job logs.
#   tools/annotate_xctest_failures.sh <xcodebuild.log> <annotation title>
set -uo pipefail
LOG=$1 TITLE=$2
[ -f "$LOG" ] || exit 0
grep -E ': error: -\[|^Test Case .* failed \(' "$LOG" | sort -u | head -20 | while IFS= read -r line; do
    line=${line//%/%25}
    line=${line//$'\r'/}
    echo "::error title=${TITLE}::${line}"
done
exit 0
