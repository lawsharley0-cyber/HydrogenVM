#!/usr/bin/env python3
"""Stable, structural fingerprints for HydrogenVM failure evidence.

A fingerprint identifies a hypothesis to retest, not a proven root cause. It
intentionally excludes source commits, wall-clock time, addresses and counts so
that equivalent failures can be compared across builds and attempts.
"""
from __future__ import annotations

import hashlib
import json
import re

FINGERPRINT_VERSION = 1

OUTCOME_CLASSES = {
    "load": "import_failure",
    "import": "import_failure",
    "crash": "crash_unattributed",
    "guest_crash": "guest_crash",
    "host_crash": "host_crash",
    "hang": "liveness_failure",
    "close": "lifecycle_failure",
    "picture": "behavioral_failure",
    "offscreen": "behavioral_failure",
    "behavioral": "behavioral_failure",
    "silent": "behavioral_failure",
    "fault": "guest_handled_fault",
    "missing": "missing_api",
    "unimplemented": "unimplemented_import",
    "infrastructure": "infrastructure_error",
    "error": "runtime_error",
    "warning": "warning",
}

_ADDRESS = re.compile(r"(?<![\w])0x[0-9a-fA-F]+(?![\w])")
_UUID = re.compile(
    r"(?<![\w])[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
    r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}(?![\w])"
)
_TIMESTAMP = re.compile(
    r"(?<!\w)(?:\d{4}-\d{2}-\d{2}[T ][0-9:.]+Z?|\d{2}:\d{2}:\d{2}(?:\.\d+)?)(?!\w)"
)
_NUMBER = re.compile(r"(?<![\w.])\d+(?:\.\d+)?(?![\w.])")
_WINDOWS_PATH = re.compile(r"(?<!\w)[A-Za-z]:[\\/][^\s,'\"\]]+")
_POSIX_PATH = re.compile(r"(?<![\w:])/(?:[^/\s]+/)+[^\s,'\"\]]*")
_VOLATILE = re.compile(
    r"\[(?:signature|sha|commit|run|attempt|duration|elapsed|pid|tid)=[^\]]*\]",
    re.IGNORECASE,
)
_SPACE = re.compile(r"\s+")


def normalize_structure(value: object, *, limit: int = 400) -> str:
    """Remove volatile values while retaining API, selector and stack structure."""
    if value is None:
        return ""
    if isinstance(value, (list, tuple)):
        value = " <- ".join(str(item) for item in value[:6])
    text = str(value).replace("\\", "/")
    text = _VOLATILE.sub("[volatile]", text)
    text = _UUID.sub("<uuid>", text)
    text = _TIMESTAMP.sub("<time>", text)
    text = _WINDOWS_PATH.sub("<path>", text)
    text = _POSIX_PATH.sub("<path>", text)
    text = _ADDRESS.sub("<address>", text)
    text = _NUMBER.sub("<number>", text)
    return _SPACE.sub(" ", text).strip()[:limit]


def outcome_class(kind: str, detail: str = "", exception_kind: str = "") -> str:
    """Classify evidence without silently attributing an ambiguous host/guest crash."""
    combined = f"{kind} {exception_kind} {detail}".lower()
    if kind in {"infrastructure", "infrastructure_error"} or "runner-infrastructure" in combined:
        return "infrastructure_error"
    if "host" in combined and ("crash" in combined or "fault" in combined):
        return "host_crash"
    if "guest" in combined and ("crash" in combined or "fault" in combined):
        return "guest_crash"
    if "teardown" in combined or "quitting" in combined or "relaunch" in combined:
        return "lifecycle_failure"
    if "import" in combined or kind == "load":
        return "import_failure"
    if kind == "crash" and ("signal" in combined or "exception" in combined or "guest" in combined):
        return "guest_crash"
    return OUTCOME_CLASSES.get(kind, "behavioral_failure")


def fingerprint(
    kind: str,
    operation: object,
    *,
    stack: object = "",
    renderer: str = "any",
    detail: str = "",
    exception_kind: str = "",
    build_semantics: str = "",
) -> dict:
    """Return a versioned fingerprint with a deterministic SHA-256 identity."""
    category = outcome_class(kind, detail, exception_kind)
    renderer = renderer if renderer in {"software", "gpu"} else "any"
    body = {
        "version": FINGERPRINT_VERSION,
        "outcomeClass": category,
        "operation": normalize_structure(operation),
        "stackPrefix": normalize_structure(stack, limit=600),
        "renderer": renderer,
        "buildSemantics": normalize_structure(build_semantics, limit=120),
    }
    encoded = json.dumps(body, ensure_ascii=False, sort_keys=True, separators=(",", ":"))
    return {"id": "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest(), **body}


def cluster_identity(manual_cluster: str = "", fingerprint_id: str = "") -> str:
    """Give reviewed split/merge groups stable IDs separate from raw fingerprints."""
    value = manual_cluster or fingerprint_id
    if not value:
        raise ValueError("a cluster needs a manual name or fingerprint ID")
    return "cluster:" + hashlib.sha256(value.encode("utf-8")).hexdigest()[:24]
