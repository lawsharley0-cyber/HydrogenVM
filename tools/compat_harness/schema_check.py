"""Structural check for harness reports.

``schema/report.schema.json`` is the document a JSON Schema validator can
load. This module enforces the same required fields with the standard
library so CI does not need a schema package. It is a conformance check,
not a full Draft 2020-12 implementation.
"""
from __future__ import annotations

import json
import os

from .manifest import package_dir

_APP_STATUSES = frozenset({"skipped", "error", "ran", "crashed", "load-error", "timeout"})
_CASE_STATUSES = frozenset({"pass", "fail", "skip"})


def schema_path() -> str:
    return os.path.join(package_dir(), "schema", "report.schema.json")


def load_schema() -> dict:
    with open(schema_path(), encoding="utf-8") as f:
        return json.load(f)


def validate_report(report: object) -> list[str]:
    errors: list[str] = []
    if not isinstance(report, dict):
        return ["report must be an object"]
    _require(report, {
        "schemaVersion": int,
        "kind": str,
        "generatedAt": str,
        "git": dict,
        "host": dict,
        "limits": dict,
        "manifest": dict,
        "suites": dict,
        "clusters": list,
        "summary": dict,
    }, "report", errors)
    if report.get("kind") not in (None, "hvm-compat-report"):
        errors.append("report.kind must be hvm-compat-report")
    if report.get("schemaVersion") not in (None, 1):
        errors.append("report.schemaVersion must be 1")
    host = report.get("host")
    if isinstance(host, dict) and "pageSize" not in host:
        errors.append("report.host.pageSize is required")
    suites = report.get("suites") if isinstance(report.get("suites"), dict) else {}
    for name in ("unit", "python", "apps"):
        if name not in suites:
            errors.append(f"report.suites.{name} is required")
    for name in ("unit", "python"):
        suite = suites.get(name)
        if isinstance(suite, dict):
            _validate_cases(suite.get("cases"), f"suites.{name}", errors)
    apps = suites.get("apps") if isinstance(suites.get("apps"), dict) else {}
    rows = apps.get("apps") if isinstance(apps, dict) else None
    if not isinstance(rows, list):
        errors.append("report.suites.apps.apps must be a list")
    else:
        for i, app in enumerate(rows):
            _validate_app(app, i, errors)
    clusters = report.get("clusters")
    if isinstance(clusters, list):
        for i, item in enumerate(clusters):
            if not isinstance(item, dict):
                errors.append(f"clusters[{i}] must be an object")
                continue
            for key in ("kind", "key", "likelyOwner", "apps"):
                if key not in item:
                    errors.append(f"clusters[{i}].{key} is required")
            if "apps" in item and not isinstance(item["apps"], list):
                errors.append(f"clusters[{i}].apps must be a list")
    summary = report.get("summary")
    if isinstance(summary, dict) and not isinstance(summary.get("ok"), bool):
        errors.append("report.summary.ok must be a boolean")
    schema = load_schema()
    required = schema.get("required") or []
    for key in required:
        if key not in report:
            errors.append(f"schema required field missing from checker input: {key}")
    return errors


def validate_diff(diff: object) -> list[str]:
    errors: list[str] = []
    if not isinstance(diff, dict):
        return ["diff must be an object"]
    _require(diff, {
        "schemaVersion": int,
        "kind": str,
        "ok": bool,
        "regressions": list,
        "improvements": list,
        "attention": list,
        "informational": list,
    }, "diff", errors)
    if diff.get("kind") not in (None, "hvm-compat-diff"):
        errors.append("diff.kind must be hvm-compat-diff")
    return errors


def _validate_cases(cases, where: str, errors: list[str]) -> None:
    if not isinstance(cases, list):
        errors.append(f"{where}.cases must be a list")
        return
    for i, case in enumerate(cases):
        if not isinstance(case, dict):
            errors.append(f"{where}.cases[{i}] must be an object")
            continue
        if not isinstance(case.get("id"), str):
            errors.append(f"{where}.cases[{i}].id must be a string")
        if case.get("status") not in _CASE_STATUSES:
            errors.append(f"{where}.cases[{i}].status is invalid")


def _validate_app(app, index: int, errors: list[str]) -> None:
    where = f"suites.apps.apps[{index}]"
    if not isinstance(app, dict):
        errors.append(f"{where} must be an object")
        return
    for key in ("id", "status", "sha256", "launchSuccess", "crashed", "frame", "audio", "clusters"):
        if key not in app:
            errors.append(f"{where}.{key} is required")
    if app.get("status") not in _APP_STATUSES:
        errors.append(f"{where}.status is invalid")
    if not isinstance(app.get("launchSuccess"), bool) or not isinstance(app.get("crashed"), bool):
        errors.append(f"{where} launchSuccess and crashed must be booleans")
    frame = app.get("frame")
    if isinstance(frame, dict) and not isinstance(frame.get("signal"), str):
        errors.append(f"{where}.frame.signal must be a string")
    audio = app.get("audio")
    if isinstance(audio, dict) and not isinstance(audio.get("signal"), str):
        errors.append(f"{where}.audio.signal must be a string")


def _require(obj: dict, types: dict, where: str, errors: list[str]) -> None:
    for key, kind in types.items():
        if key not in obj:
            errors.append(f"{where}.{key} is required")
        elif not isinstance(obj[key], kind):
            errors.append(f"{where}.{key} has the wrong type")
