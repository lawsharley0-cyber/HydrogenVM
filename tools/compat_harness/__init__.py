"""Versioned compatibility harness for HydrogenVM.

This package is intentionally not named ``compat``. ``tools/compat.py`` is the
existing interactive scanner, and ``tools/clash_royale_probe.py`` imports it
with ``from compat import macho_info``. A ``tools/compat/`` package would
shadow that module.
"""

SCHEMA_VERSION = 1
