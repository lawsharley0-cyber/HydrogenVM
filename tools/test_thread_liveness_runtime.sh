#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
out="${1:-build/thread-liveness-runtime-sanitized}"
cmake -S . -B "$out" -G Ninja -DCMAKE_CXX_COMPILER="${CXX:-clang++}" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS_RELEASE='-O1 -DNDEBUG' \
  -DCMAKE_CXX_FLAGS='-g1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld
cmake --build "$out" --target hvm -j 2
HVM_THREAD_LIVENESS=1 python3 tools/hvm_testlab.py run --hvm "$out/hvm" \
  --out "$out/testlab" --case TL-L
"$out/hvm" run build/ios/TestLab.app --home "$out/probe-home" --run-for 3 \
  --graceful --thread-liveness --report "$out/probe.json" -- --run TL-L
python3 - "$out/probe.json" <<'PY'
import json, sys
report = json.load(open(sys.argv[1]))
samples = report['threadLiveness']
assert report['crashed'] is False and report['exitStatus'] == 0, report.get('crashReport')
assert samples[0]['event'] == 'run-start' and samples[-1]['event'] == 'run-end'
assert sum(s['event'] == 'interval' for s in samples) >= 2, samples
assert any(t['snapshotValid'] for s in samples for t in s['threads']), samples
print('ok actual runtime liveness sampler startup, interval and shutdown under ASan/UBSan')
PY
