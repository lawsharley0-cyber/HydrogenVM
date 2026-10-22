#!/usr/bin/env python3
"""Entry point for the HydrogenVM compatibility harness.

Usage:
  python3 tools/hvm_compat.py run --out build/compat-report
  python3 tools/hvm_compat.py diff --previous build/compat-report/report.json
  python3 tools/hvm_compat.py record build/compat-report/report.json
  python3 tools/hvm_compat.py issues build/compat-report/report.json
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from compat_harness.cli import main

if __name__ == "__main__":
    raise SystemExit(main())
