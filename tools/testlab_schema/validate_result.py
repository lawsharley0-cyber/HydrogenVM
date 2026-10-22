#!/usr/bin/env python3
"""Validate a testlab result JSON against testlab_result.schema.json.

Uses the same lightweight validator pattern as tools/compat_harness/schema_check.py —
no external jsonschema dependency required.

usage: python3 tools/testlab_schema/validate_result.py result.json
"""
import json
import re
import sys
from pathlib import Path

SCHEMA_PATH = Path(__file__).parent / "testlab_result.schema.json"

VALID_STATUSES = {"passed", "failed", "skipped", "interrupted"}
VALID_KINDS = {"hvm-testlab-result", "hvm-testlab-host-report"}
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def _fail(path: str, msg: str) -> None:
    print(f"FAIL  {path}: {msg}", file=sys.stderr)


def validate_case(case: dict, idx: int, errors: list) -> None:
    path = f"cases[{idx}]"
    for req in ("id", "version", "status"):
        if req not in case:
            errors.append(f"{path}: missing required field '{req}'")
    status = case.get("status")
    if status not in VALID_STATUSES:
        errors.append(f"{path}.status: '{status}' not one of {sorted(VALID_STATUSES)}")
    version = case.get("version")
    if not isinstance(version, int):
        errors.append(f"{path}.version: must be integer, got {type(version).__name__}")
    if status in ("failed", "skipped", "interrupted") and not case.get("reason"):
        errors.append(f"{path}: status '{status}' requires a non-empty 'reason'")
    dur = case.get("durationMs")
    if dur is not None and not isinstance(dur, (int, float)):
        errors.append(f"{path}.durationMs: must be number or null")


def validate(data: dict) -> list:
    errors = []
    for req in ("schemaVersion", "kind", "generatedAt", "runId", "appVersion", "cases"):
        if req not in data:
            errors.append(f"top-level: missing required field '{req}'")

    if data.get("schemaVersion") != 1:
        errors.append(f"schemaVersion: expected 1, got {data.get('schemaVersion')!r}")

    kind = data.get("kind")
    if kind not in VALID_KINDS:
        errors.append(f"kind: '{kind}' not one of {sorted(VALID_KINDS)}")

    sha = data.get("artifactSha256")
    if sha is not None and not SHA256_RE.match(sha):
        errors.append(f"artifactSha256: invalid SHA-256 '{sha}'")

    cases = data.get("cases")
    if not isinstance(cases, list):
        errors.append("cases: must be an array")
    else:
        for i, c in enumerate(cases):
            validate_case(c, i, errors)

    host = data.get("host", {})
    if not isinstance(host, dict):
        errors.append("host: must be an object")
    else:
        ec = host.get("emulatorCommit")
        if ec is not None and not isinstance(ec, str):
            errors.append("host.emulatorCommit: must be string or null — do not invent")

    return errors


def main() -> int:
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} result.json", file=sys.stderr)
        return 2
    path = sys.argv[1]
    try:
        data = json.loads(Path(path).read_text())
    except Exception as e:
        print(f"FAIL  could not read '{path}': {e}", file=sys.stderr)
        return 1
    errors = validate(data)
    if errors:
        for e in errors:
            _fail(path, e)
        print(f"validation FAILED: {len(errors)} error(s)", file=sys.stderr)
        return 1
    n_cases = len(data.get("cases", []))
    print(f"ok  {path}  ({n_cases} cases, schemaVersion=1)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
