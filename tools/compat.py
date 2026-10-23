#!/usr/bin/env python3
"""Compatibility alias: scan inputs through the manifest harness."""
from compat_harness.scan import main
from compat_harness.macho import macho_info  # public compatibility API used by clash_royale_probe

if __name__ == '__main__':
    raise SystemExit(main())
