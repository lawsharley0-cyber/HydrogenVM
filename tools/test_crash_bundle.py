#!/usr/bin/env python3
"""Integration oracle for the opt-in Test Lab diagnostic crash (issue #21)."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument('--hvm', default='build/hvm')
parser.add_argument('--ipa', default='build/ios/TestLab.ipa')
args = parser.parse_args()
with tempfile.TemporaryDirectory() as directory:
    report = Path(directory) / 'report.json'
    command = [args.hvm, 'run', args.ipa, '--home', directory, '--report', str(report), '--', '--force-crash']
    results = []
    for _ in range(2):
        run = subprocess.run(command, capture_output=True, text=True, timeout=30)
        data = json.loads(report.read_text())
        assert data['crashed'], (run.returncode, run.stderr)
        assert 'signature' in data, 'FAIL: --report has no signature'
        bundle = data['crashBundle']
        key = bundle['exceptionKind'] + '\n' + bundle['location'] + '\n' + bundle['detail'].split(' (called from ')[0]
        assert data['signature'] == hashlib.sha1(key.encode()).hexdigest()[:12]
        assert len(bundle['registers']) == 16
        assert bundle['host']['pageSize'] > 0
        assert len(bundle['executableSHA256']) == 64
        assert bundle['backtrace'] and 'TestLab+' in bundle['backtrace'][0]
        assert 0 < len(bundle['logs']) <= 2000
        assert bundle['bundleId'] and 'guestMIPS' in bundle['perf']
        results.append(data['signature'])
    assert len(set(results)) == 1, results
    print(f'PASS: forced Test Lab crash; signature={results[0]}; stable across two launches; registers, image offsets, logs, hash, page size and perf present')
