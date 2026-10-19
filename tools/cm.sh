#!/usr/bin/env bash
# Quick CoreMark: best of N runs (default 5) of build/ios/CoreMark.app.
#   tools/cm.sh [hvm-binary] [runs] [iterations]
HVM=${1:-build/hvm} N=${2:-5} IT=${3:-3000}
best=0
for i in $(seq 1 "$N"); do
  s=$("$HVM" run build/ios/CoreMark.app --quiet -- 0x0 0x0 0x66 "$IT" 2>/dev/null | sed -n 's/Iterations\/Sec *: *//p')
  best=$(python3 -c "print(max($best, ${s:-0}))")
  printf '%s ' "$s"
done
echo "=> best $best"
