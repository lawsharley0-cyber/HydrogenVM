#!/usr/bin/env python3
"""Drive an actual guest termination timeout; a timed stop must not mask failure."""
import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--hvm', type=Path, required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    with tempfile.TemporaryDirectory(prefix='hvm-watchdog-') as directory:
        out = Path(directory)
        subprocess.run([sys.executable, str(root / 'tools/ios_build.py'), 'app',
                        '--name', 'ShutdownWatchdog', '--out', str(out),
                        str(root / 'tests/ios/apps/shutdown_watchdog/main.m')], check=True, cwd=root)
        report = out / 'report.json'
        result = subprocess.run([str(args.hvm.resolve()), 'run', str(out / 'ShutdownWatchdog.app'),
            '--home', str(out / 'home'), '--run-for', '1', '--graceful', '--report', str(report)],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=25, cwd=root)
        record = json.loads(report.read_text())
        required = ['WATCHDOG_READY', 'WATCHDOG_TERMINATION_ENTERED']
        if (any(marker not in result.stdout for marker in required) or
            result.returncode != 70 or record.get('exitStatus') != 70 or record.get('crashed') is not True or
            'watchdog' not in record.get('crashReport', '').lower()):
            print(result.stdout)
            print('FAIL termination watchdog must produce host/report status 70 and a recorded crash;',
                  'actual', result.returncode, record.get('exitStatus'), record.get('crashed'))
            return 1
        print('ok actual guest saving watchdog cannot be masked by timed-stop success')
        return 0

if __name__ == '__main__':
    raise SystemExit(main())
