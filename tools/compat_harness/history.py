"""Store each harness report under ``docs/dev-system/reports``.

A run is a JSON file at ``<platform>/<UTC timestamp>-<short sha>.json``.
``index.json`` lists those runs. ``latest.json`` points at the last row.
The committed baseline is a separate file. Recording a run does not replace it.

``previous`` is the preceding row in the index. A report that is not in the
index is compared with the last row. An empty index has no previous report.
"""

import datetime
import json
import os
import re

from .diffing import diff_reports
from .schema_check import validate_report

INDEX_SCHEMA_VERSION = 1
_KIND_INDEX = "hvm-compat-report-index"
_KIND_POINTER = "hvm-compat-report-pointer"
_SHA = re.compile(r"[0-9a-fA-F]{7,40}")


class HistoryError(Exception):
    pass


def reports_dir(repo: str) -> str:
    return os.path.join(os.path.abspath(repo), "docs", "dev-system", "reports")


def index_path(repo: str) -> str:
    return os.path.join(reports_dir(repo), "index.json")


def latest_path(repo: str) -> str:
    return os.path.join(reports_dir(repo), "latest.json")


def record_report(report: dict, repo: str) -> dict:
    """Copy ``report`` into the index. The same bytes are not stored twice."""
    _require_report(report)
    root = reports_dir(repo)
    os.makedirs(root, exist_ok=True)
    rel, dest = _allocate_path(report, root)
    _ensure_inside(dest, root)
    if not (os.path.isfile(dest) and _load(dest) == report):
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        _dump(dest, report)
    entry = _entry(report, rel)
    _upsert(repo, entry)
    return entry


def load_index(repo: str) -> dict:
    path = index_path(repo)
    if not os.path.isfile(path):
        return {"schemaVersion": INDEX_SCHEMA_VERSION, "kind": _KIND_INDEX, "runs": []}
    data = _load(path)
    if not isinstance(data, dict) or not isinstance(data.get("runs"), list):
        raise HistoryError(f"{path} is not an index")
    data.setdefault("schemaVersion", INDEX_SCHEMA_VERSION)
    data.setdefault("kind", _KIND_INDEX)
    return data


def previous_report_path(repo: str, current_path: str) -> str | None:
    """Path of the indexed report before ``current_path``.

    An index path uses the preceding row. A file that is not indexed uses the
    last row, unless its JSON is that last row (the usual case after
    ``record`` copied ``build/compat-report/report.json``). That copy is the
    row itself, so the previous run is the one before it. The first row, and
    an empty index, have none.
    """
    runs = load_index(repo).get("runs") or []
    if not runs:
        return None
    rel = _matching_rel(repo, current_path, runs)
    if rel is None:
        last = _abs_run(repo, runs[-1])
        if _same_json(current_path, last):
            if len(runs) == 1:
                return None
            return _abs_run(repo, runs[-2])
        return last
    idx = None
    for i, row in enumerate(runs):
        if row.get("path") == rel:
            idx = i
    if idx is None or idx == 0:
        return None
    return _abs_run(repo, runs[idx - 1])


def index_run(report: dict, repo: str, *, record: bool, diff_previous: bool, current_path: str) -> dict:
    """Diff against the previous indexed report, then optionally record this one.

    Diffing happens first so the new row is not its own previous report.
    """
    result: dict = {"recorded": None, "previousDiff": None, "previousMissing": False}
    if diff_previous:
        prev = previous_report_path(repo, current_path)
        if prev is None:
            result["previousMissing"] = True
        else:
            baseline = _load(prev)
            if not isinstance(baseline, dict):
                raise HistoryError(f"{prev} is not a JSON object")
            result["previousDiff"] = diff_reports(baseline, report)
    if record:
        result["recorded"] = record_report(report, repo)
    return result


def _require_report(report: dict) -> None:
    if not isinstance(report, dict):
        raise HistoryError("report must be a JSON object")
    errors = validate_report(report)
    if errors:
        raise HistoryError("report failed schema check: " + "; ".join(errors))


def _allocate_path(report: dict, root: str) -> tuple[str, str]:
    platform = _platform(report)
    stamp = _stamp(report)
    short = _short(report)
    directory = os.path.join(root, platform)
    for n in range(1, 100):
        suffix = "" if n == 1 else f"-{n}"
        name = f"{stamp}-{short}{suffix}.json"
        rel = f"{platform}/{name}"
        dest = os.path.join(directory, name)
        if not os.path.isfile(dest):
            return rel, dest
        if _load(dest) == report:
            return rel, dest
    raise HistoryError("too many report filename collisions")


def _entry(report: dict, rel: str) -> dict:
    git = report.get("git") if isinstance(report.get("git"), dict) else {}
    sha = git.get("commit") or git.get("short") or ""
    if not isinstance(sha, str):
        sha = str(sha)
    summary = report.get("summary") if isinstance(report.get("summary"), dict) else {}
    return {
        "sha": sha,
        "short": _short(report),
        "date": _date(report),
        "schemaVersion": report["schemaVersion"],
        "platform": _platform(report),
        "path": rel,
        "summary": summary,
    }


def _upsert(repo: str, entry: dict) -> None:
    index = load_index(repo)
    runs = list(index.get("runs") or [])
    replaced = False
    for i, row in enumerate(runs):
        if row.get("path") == entry["path"]:
            runs[i] = entry
            replaced = True
            break
    if not replaced:
        runs.append(entry)
    _dump(index_path(repo), {
        "schemaVersion": INDEX_SCHEMA_VERSION,
        "kind": _KIND_INDEX,
        "runs": runs,
    })
    latest = runs[-1]
    _dump(latest_path(repo), {
        "schemaVersion": INDEX_SCHEMA_VERSION,
        "kind": _KIND_POINTER,
        "path": latest["path"],
        "sha": latest["sha"],
        "date": latest["date"],
        "platform": latest["platform"],
        "reportSchemaVersion": latest["schemaVersion"],
        "summary": latest["summary"],
    })


def _platform(report: dict) -> str:
    host = report.get("host") if isinstance(report.get("host"), dict) else {}
    system = str(host.get("system") or "unknown")
    machine = str(host.get("machine") or "unknown")
    slug = re.sub(r"[^a-z0-9._-]+", "-", f"{system}-{machine}".lower()).strip("-._")
    return slug or "unknown"


def _short(report: dict) -> str:
    git = report.get("git") if isinstance(report.get("git"), dict) else {}
    short = str(git.get("short") or "")
    commit = str(git.get("commit") or "")
    token = short if _SHA.fullmatch(short) else commit[:12]
    token = token.lower()
    if not _SHA.fullmatch(token):
        return "unknown"
    return token


def _date(report: dict) -> str:
    raw = report.get("generatedAt")
    if isinstance(raw, str) and raw.strip():
        return raw.strip()
    return datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0).isoformat()


def _stamp(report: dict) -> str:
    raw = _date(report)
    text = raw[:-1] + "+00:00" if raw.endswith("Z") else raw
    try:
        parsed = datetime.datetime.fromisoformat(text)
    except ValueError as e:
        raise HistoryError(f"unusable generatedAt {raw!r}") from e
    if parsed.tzinfo is None:
        parsed = parsed.replace(tzinfo=datetime.timezone.utc)
    parsed = parsed.astimezone(datetime.timezone.utc).replace(microsecond=0)
    return parsed.strftime("%Y%m%dT%H%M%SZ")


def _matching_rel(repo: str, current_path: str, runs: list) -> str | None:
    if not current_path:
        return None
    try:
        current_real = os.path.realpath(current_path)
    except OSError:
        return None
    root = os.path.realpath(reports_dir(repo))
    found = None
    for row in runs:
        rel = row.get("path")
        if not isinstance(rel, str) or not _safe_rel(rel):
            continue
        candidate = os.path.realpath(os.path.join(root, rel))
        try:
            if os.path.commonpath([root, candidate]) != root:
                continue
        except ValueError:
            continue
        if candidate == current_real:
            found = rel
    return found


def _abs_run(repo: str, row: dict) -> str:
    rel = row.get("path")
    if not isinstance(rel, str) or not _safe_rel(rel):
        raise HistoryError("index row has an unsafe path")
    root = os.path.realpath(reports_dir(repo))
    dest = os.path.realpath(os.path.join(root, rel))
    if not _inside(dest, root):
        raise HistoryError("index row points outside the reports directory")
    return dest


def _safe_rel(rel: str) -> bool:
    if not rel or rel.startswith("/") or "\\" in rel:
        return False
    parts = rel.split("/")
    return all(part not in ("", ".", "..") for part in parts)


def _inside(path: str, root: str) -> bool:
    real_root = os.path.realpath(root)
    real_path = os.path.realpath(path)
    try:
        return os.path.commonpath([real_root, real_path]) == real_root
    except ValueError:
        return False


def _ensure_inside(path: str, root: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not _inside(path, root):
        raise HistoryError(f"refusing to write outside {os.path.realpath(root)}")


def _same_json(left: str, right: str) -> bool:
    if not left or not os.path.isfile(left) or not os.path.isfile(right):
        return False
    try:
        return _load(left) == _load(right)
    except (OSError, json.JSONDecodeError):
        return False


def _load(path: str):
    with open(path, encoding="utf-8") as fh:
        return json.load(fh)


def _dump(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as fh:
        json.dump(data, fh, indent=2)
        fh.write("\n")
