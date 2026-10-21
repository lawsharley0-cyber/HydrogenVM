"""Structural check for harness reports.

``schema/report.schema.json`` is Draft 2020-12. This module walks that
document — type, const, enum, pattern, required, properties, items, and
``$ref`` — with the standard library so CI does not need a schema package.
Every constraint the report schema states is enforced. Keywords this schema
does not use (``if``/``then``, ``format``, ``unevaluatedProperties``) are not.
"""
from __future__ import annotations

import json
import os
import re

from .manifest import package_dir

_SKIP = frozenset({"$schema", "$id", "$defs", "$comment", "title", "description"})


def schema_path() -> str:
    return os.path.join(package_dir(), "schema", "report.schema.json")


def load_schema() -> dict:
    with open(schema_path(), encoding="utf-8") as f:
        return json.load(f)


def validate_report(report: object) -> list[str]:
    errors: list[str] = []
    if not isinstance(report, dict):
        return ["report must be an object"]
    _check(report, load_schema(), "report", load_schema(), errors)
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


def _check(value, schema: dict, path: str, root: dict, errors: list[str]) -> None:
    if "$ref" in schema:
        _check(value, _resolve(root, schema["$ref"]), path, root, errors)
        local = {k: v for k, v in schema.items() if k != "$ref" and k not in _SKIP}
        if local:
            _check(value, local, path, root, errors)
        return
    if "type" in schema:
        types = schema["type"]
        if isinstance(types, str):
            types = [types]
        if not any(_type_ok(value, kind) for kind in types):
            errors.append(f"{path} has the wrong type")
            return
    if "const" in schema and not _const_ok(value, schema["const"]):
        errors.append(f"{path} must be {schema['const']!r}")
        return
    if "enum" in schema and not any(_const_ok(value, item) for item in schema["enum"]):
        errors.append(f"{path} is not an allowed value")
        return
    if "pattern" in schema and isinstance(value, str) and re.search(schema["pattern"], value) is None:
        errors.append(f"{path} does not match {schema['pattern']}")
        return
    if schema.get("additionalProperties") is False and isinstance(value, dict):
        allowed = set((schema.get("properties") or {}))
        for key in value:
            if key not in allowed:
                errors.append(f"{path}.{key} is not allowed")
    if isinstance(value, dict):
        for key in schema.get("required") or []:
            if key not in value:
                errors.append(f"{path}.{key} is required")
        props = schema.get("properties") or {}
        for key, sub in props.items():
            if key in value:
                _check(value[key], sub, f"{path}.{key}", root, errors)
    if isinstance(value, list) and "items" in schema:
        item_schema = schema["items"]
        if isinstance(item_schema, dict):
            for index, item in enumerate(value):
                _check(item, item_schema, f"{path}[{index}]", root, errors)


def _resolve(root: dict, ref: str) -> dict:
    if not ref.startswith("#/"):
        raise ValueError(f"unsupported schema ref {ref}")
    node = root
    for part in ref[2:].split("/"):
        part = part.replace("~1", "/").replace("~0", "~")
        node = node[part]
    if not isinstance(node, dict):
        raise ValueError(f"schema ref {ref} is not an object")
    return node


def _type_ok(value, kind: str) -> bool:
    if kind == "object":
        return isinstance(value, dict)
    if kind == "array":
        return isinstance(value, list)
    if kind == "string":
        return isinstance(value, str)
    if kind == "boolean":
        return isinstance(value, bool)
    if kind == "integer":
        return isinstance(value, int) and not isinstance(value, bool)
    if kind == "number":
        return isinstance(value, (int, float)) and not isinstance(value, bool)
    if kind == "null":
        return value is None
    return False


def _const_ok(value, expected) -> bool:
    if isinstance(expected, bool) or isinstance(value, bool):
        return isinstance(value, bool) and isinstance(expected, bool) and value is expected
    return value == expected


def _require(obj: dict, types: dict, where: str, errors: list[str]) -> None:
    for key, kind in types.items():
        if key not in obj:
            errors.append(f"{where}.{key} is required")
        elif kind is int:
            if not isinstance(obj[key], int) or isinstance(obj[key], bool):
                errors.append(f"{where}.{key} has the wrong type")
        elif not isinstance(obj[key], kind):
            errors.append(f"{where}.{key} has the wrong type")
