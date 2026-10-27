#!/usr/bin/env python3
"""Convert HydrogenVM desktop/device reports into hvm-evidence/v1 bundles.

Examples (use isolated Python when reports came from an upload):
  python3 -I tools/hvm_evidence.py --probe-summary ~/probe/latest/summary.json --out build/evidence-probe
  python3 -I tools/hvm_evidence.py --device-report latest_batch_report.json --out build/evidence-device

The output is an index of private evidence, not a redistribution bundle. Rich
artifacts stay where they are and are identified by path, size and SHA-256.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

# Isolated Python (-I) also removes the script directory from sys.path. Import
# only our trusted sibling module explicitly; report paths remain data-only.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from hvm_fingerprint import cluster_identity, fingerprint  # noqa: E402

SCHEMA = "hvm-evidence/v1"
KIND = "hvm-evidence-bundle"
ARTIFACT_NAMES = ("run.log", "report.json", "shot.png", "script.txt")
MEDIA_TYPES = {
    ".json": "application/json",
    ".log": "text/plain",
    ".txt": "text/plain",
    ".png": "image/png",
}
CHECK_SEVERITY = {
    "Launches": 100,
    "Shows a picture": 80,
    "Game view on screen": 70,
    "Responds to touch": 60,
    "Background & resume": 55,
    "Closes cleanly": 45,
    "Relaunch": 55,
    "Missing APIs": 40,
    "Handled faults": 50,
    "Audio output": 30,
    "Music": 20,
    "Sound effects": 20,
    "Speed": 20,
}


def _json_text(value: object) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def _load_json(path: Path) -> dict:
    value = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise ValueError(f"{path}: expected a JSON object")
    return value


def _build(raw: dict | None = None) -> dict:
    raw = raw or {}
    return {
        **raw,
        "commit": raw.get("commit"),
        "dirty": raw.get("dirty") if isinstance(raw.get("dirty"), bool) else None,
        "configuration": str(raw.get("configuration") or "unknown"),
        "binaryMatch": "verified" if raw.get("binaryMatch") == "verified" else "unverified",
    }


def _host(raw: dict | None = None, *, device: bool = False) -> dict:
    raw = raw or {}
    page_size = raw.get("pageSize")
    return {
        **raw,
        "kind": str(raw.get("kind") or ("iphone" if device else "desktop")),
        "os": str(raw.get("os") or "unknown"),
        "pageSize": page_size if isinstance(page_size, int) and page_size > 0 else None,
    }


def current_source_provenance() -> tuple[dict, dict]:
    """Record the checkout and host without claiming the binary matches source."""
    commit, dirty = None, None
    repo = Path(__file__).resolve().parent.parent
    try:
        commit = subprocess.run(
            ["git", "rev-parse", "--verify", "HEAD"], cwd=repo,
            check=True, capture_output=True, text=True, timeout=10,
        ).stdout.strip()
        dirty = bool(subprocess.run(
            ["git", "status", "--porcelain"], cwd=repo,
            check=True, capture_output=True, text=True, timeout=10,
        ).stdout.strip())
    except (OSError, subprocess.SubprocessError):
        pass
    try:
        page_size = os.sysconf("SC_PAGE_SIZE")
    except (AttributeError, OSError, ValueError):
        page_size = None
    build = _build({"commit": commit, "dirty": dirty, "commitBasis": "source-checkout"})
    return build, _host({"kind": "desktop", "os": platform.platform(), "pageSize": page_size})


def make_observation(kind: str, status: str, detail: str, *, strength: str = "counter") -> dict:
    if status == "skip":
        status = "not_run"
    allowed = {"pass", "warn", "fail", "not_run", "not_applicable", "inconclusive", "infrastructure_error"}
    if status not in allowed:
        raise ValueError(f"unsupported observation status: {status}")
    return {"kind": kind, "strength": strength, "status": status, "detail": str(detail)[:1000]}


def make_issue(
    kind: str,
    detail: object,
    *,
    severity: int,
    renderer: str = "any",
    stack: object = "",
    exception_kind: str = "",
    operation: object | None = None,
) -> dict:
    detail = str(detail or kind)[:2000]
    fp = fingerprint(
        kind,
        detail if operation is None else operation,
        stack=stack,
        renderer=renderer,
        detail=detail,
        exception_kind=exception_kind,
    )
    return {
        "kind": kind,
        "severity": int(severity),
        "detail": detail,
        "fingerprint": fp,
        "confidence": "discovery_only",
    }


def _outcome(issues: list[dict]) -> dict:
    if not issues:
        return {"class": "no_failure_observed", "status": "pass", "milestone": None}
    worst = max(issues, key=lambda issue: (issue["severity"], issue["fingerprint"]["id"]))
    category = worst["fingerprint"]["outcomeClass"]
    status = "infrastructure_error" if category == "infrastructure_error" else "fail"
    if worst["severity"] <= 20:
        status = "warn"
    return {"class": category, "status": status, "milestone": None}


def index_artifacts(paths: list[Path]) -> list[dict]:
    """Hash files without copying or embedding their private contents."""
    artifacts = []
    seen = set()
    for path in paths:
        path = Path(path).resolve()
        if path in seen:
            continue
        seen.add(path)
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            while chunk := stream.read(1024 * 1024):
                digest.update(chunk)
                size += len(chunk)
        artifacts.append({
            "id": "sha256:" + digest.hexdigest(),
            "path": str(path),
            "size": size,
            "sha256": digest.hexdigest(),
            "mediaType": MEDIA_TYPES.get(path.suffix.lower(), "application/octet-stream"),
            "privacy": "private",
        })
    return sorted(artifacts, key=lambda item: item["path"])


def _attach_artifacts(runs: list[dict], paths: list[Path]) -> list[dict]:
    artifacts = index_artifacts(paths)
    by_path = {item["path"]: item["id"] for item in artifacts}
    for run in runs:
        run["artifacts"] = [by_path[str(Path(path).resolve())] for path in run["artifacts"]]
    return artifacts


def _base_run(run_id: str, attempt: int, source: str, build: dict, host: dict, app: dict, scenario: dict) -> dict:
    return {
        "runId": run_id,
        "attempt": max(1, int(attempt)),
        "source": source,
        "build": _build(build),
        "host": _host(host),
        "app": {
            "bundleId": str(app.get("bundleId") or "unknown"),
            "version": app.get("version"),
            "sha256": app.get("sha256"),
            **({"name": app["name"]} if app.get("name") else {}),
        },
        "scenario": {
            "id": str(scenario.get("id") or "unspecified"),
            "version": scenario.get("version", 1),
            "seed": scenario.get("seed"),
            "sandbox": scenario.get("sandbox"),
            "clock": str(scenario.get("clock") or "wall_clock_not_replayable"),
            "traceLoss": scenario.get("traceLoss") if isinstance(scenario.get("traceLoss"), int) else None,
            **({"launches": scenario["launches"]} if scenario.get("launches") else {}),
        },
        "outcome": {"class": "inconclusive", "status": "inconclusive", "milestone": None},
        "observations": [],
        "issues": [],
        "artifacts": [],
    }


def bundle_from_probe(summary: dict) -> dict:
    """Adapt existing hvm-probe summary JSON; absent provenance remains unknown."""
    if summary.get("kind") != "hvm-probe":
        raise ValueError("not an hvm-probe summary")
    runs, artifact_paths = [], []
    batch = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(summary.get("time") or "unknown"))
    build = _build(summary.get("build"))
    host = _host(summary.get("host"))
    for result in summary.get("results") or []:
        bundle_id = str(result.get("bundleId") or "unknown")
        mode = str(result.get("mode") or "software")
        attempt = int(result.get("attempt") or 1)
        run_id = f"probe:{batch}:{bundle_id}:{mode}:{attempt}"
        seed = int(hashlib.sha1(bundle_id.encode("utf-8")).hexdigest()[:8], 16)
        run = _base_run(
            run_id, attempt, "desktop-probe", build, host,
            {"bundleId": bundle_id, "version": result.get("version"), "sha256": result.get("sha256")},
            {"id": "seeded-probe", "version": 1, "seed": seed, "sandbox": "persistent-probe-home",
             "launches": result.get("launches") or summary.get("launches") or 1},
        )
        frames = int(result.get("framesPresented") or 0)
        run["observations"].append(make_observation("frame_presented", "pass" if frames else "fail", f"framesPresented={frames}"))
        mixer = result.get("mixer") or {}
        if mixer:
            active = bool(mixer.get("framesRendered"))
            audible = bool(mixer.get("audibleFrames"))
            run["observations"].append(make_observation("mixer_active", "pass" if active else "fail", f"framesRendered={mixer.get('framesRendered', 0)}"))
            run["observations"].append(make_observation("pcm_nonzero", "pass" if audible else "warn", f"audibleFrames={mixer.get('audibleFrames', 0)}"))
        for raw in result.get("issues") or []:
            kind = str(raw.get("kind") or "behavioral")
            detail = raw.get("text") or raw.get("detail") or kind
            run["issues"].append(make_issue(
                kind, detail,
                severity=int(raw.get("severity") or 20),
                renderer=mode,
                stack=result.get("signature") or result.get("crashSignature") or "",
                exception_kind=str(result.get("exceptionKind") or ""),
                operation=detail,
            ))
        if result.get("status") == "infrastructure-error" and not any(i["fingerprint"]["outcomeClass"] == "infrastructure_error" for i in run["issues"]):
            run["issues"].append(make_issue("infrastructure", "probe runner infrastructure error", severity=100, renderer=mode))
        run["outcome"] = _outcome(run["issues"])
        run_dir = result.get("dir")
        if run_dir:
            paths = [Path(run_dir) / name for name in ARTIFACT_NAMES]
            run["artifacts"] = [str(path) for path in paths if path.is_file()]
            artifact_paths.extend(Path(path) for path in run["artifacts"])
        runs.append(run)
    return build_bundle(runs, artifact_paths=artifact_paths)


def _device_apps(report: dict) -> list[dict]:
    apps = ((report.get("suites") or {}).get("apps") or {}).get("apps")
    if not isinstance(apps, list):
        raise ValueError("not a Test All Games report: suites.apps.apps is missing")
    return [app for app in apps if isinstance(app, dict)]


def bundle_from_device(report: dict) -> dict:
    """Adapt Test All Games without treating its counters as correctness proof."""
    runs = []
    build = _build(report.get("build") or report.get("emulator"))
    device = report.get("device") or {}
    host_raw = report.get("host") or {
        "kind": "iphone", "model": device.get("model"),
        "os": f"iOS {device.get('systemVersion')}" if device.get("systemVersion") else "unknown",
    }
    host = _host(host_raw, device=True)
    batch = re.sub(r"[^A-Za-z0-9_.-]+", "_", str(report.get("runId") or report.get("timestamp") or "unknown"))
    for app in _device_apps(report):
        bundle_id = str(app.get("bundleId") or "unknown")
        mode = str(app.get("mode") or app.get("renderer") or "any")
        if mode not in {"software", "gpu"}:
            mode = "any"
        explore = app.get("explore") or {}
        run = _base_run(
            f"device:{batch}:{bundle_id}:{mode}:{int(app.get('attempt') or 1)}",
            int(app.get("attempt") or 1), "iphone-test-all-games", build, host,
            {"bundleId": bundle_id, "name": app.get("name"), "version": app.get("version"), "sha256": app.get("sha256")},
            {"id": "test-all-games", "version": 1, "seed": explore.get("seed"),
             "sandbox": "throwaway-copy-unhashed", "launches": 2},
        )
        exception_kind = str(app.get("exceptionKind") or "")
        summary = str(app.get("summary") or "")
        crash_report = str(app.get("crashReport") or "")
        infrastructure = bool(app.get("infrastructureError") or exception_kind.startswith("runner-infrastructure"))
        if infrastructure:
            run["issues"].append(make_issue("infrastructure", summary or exception_kind, severity=100, renderer=mode, exception_kind=exception_kind))
        if app.get("loadError"):
            run["issues"].append(make_issue("load", app["loadError"], severity=100, renderer=mode))
        if not infrastructure and (app.get("crashed") or crash_report or exception_kind):
            detail = crash_report.splitlines()[0] if crash_report.strip() else summary or exception_kind or "crash"
            kind = "host_crash" if "host" in exception_kind.lower() else "crash"
            run["issues"].append(make_issue(kind, detail, severity=100, renderer=mode, stack=app.get("signature") or "", exception_kind=exception_kind))
        for check in app.get("checks") or []:
            name = str(check.get("name") or "unspecified-check")
            status = str(check.get("status") or "inconclusive")
            detail = str(check.get("detail") or "")
            strength = "scenario_milestone" if name in {"Responds to touch", "Background & resume", "Closes cleanly", "Relaunch"} else "counter"
            run["observations"].append(make_observation(name, status, detail, strength=strength))
            if not infrastructure and status in {"fail", "warn"}:
                severity = CHECK_SEVERITY.get(name, 40)
                if status == "warn":
                    severity = min(severity, 20)
                kind = "close" if name in {"Closes cleanly", "Relaunch"} else "behavioral"
                run["issues"].append(make_issue(kind, detail or name, severity=severity, renderer=mode, operation=f"check:{name}"))
        missing = {
            name.split(" (called from ", 1)[0].strip()
            for name in re.findall(r"Missing:\s*([^\r\n]+)", str(app.get("logs") or ""))
        }
        for name in sorted(missing):
            run["issues"].append(make_issue("missing", name, severity=40, renderer=mode))
        run["outcome"] = _outcome(run["issues"])
        runs.append(run)
    return build_bundle(runs)


def _apply_overrides(runs: list[dict], overrides: dict) -> None:
    merge = overrides.get("merge") or {}
    split = overrides.get("split") or {}
    if not isinstance(merge, dict) or not isinstance(split, dict):
        raise ValueError("overrides merge and split must be objects")
    for run in runs:
        for issue in run["issues"]:
            fp_id = issue["fingerprint"]["id"]
            split_key = f"{run['runId']}:{fp_id}"
            if split_key in split:
                issue["manualCluster"] = str(split[split_key])
            elif fp_id in merge:
                issue["manualCluster"] = str(merge[fp_id])


def _reproducibility(runs: list[dict], apps: set[str], failed_run_ids: set[str]) -> dict:
    per_app = {}
    for app in sorted(apps):
        attempts = {run["runId"] for run in runs if run["app"]["bundleId"] == app}
        failures = attempts & failed_run_ids
        if failures and failures != attempts:
            status = "flaky"
        elif len(failures) > 1:
            status = "reproduced"
        else:
            status = "untested"
        per_app[app] = {"status": status, "attempts": len(attempts), "failures": len(failures)}
    statuses = {entry["status"] for entry in per_app.values()}
    overall = "flaky" if "flaky" in statuses else "reproduced" if statuses and statuses == {"reproduced"} else "untested"
    return {"status": overall, "perApp": per_app}


def _clusters(runs: list[dict]) -> list[dict]:
    grouped = defaultdict(list)
    for run in runs:
        for issue in run["issues"]:
            manual = issue.get("manualCluster", "")
            key = cluster_identity(manual, issue["fingerprint"]["id"])
            grouped[key].append((run, issue, bool(manual)))
    clusters = []
    for cluster_id, entries in grouped.items():
        run_ids = {run["runId"] for run, _, _ in entries}
        apps = {run["app"]["bundleId"] for run, _, _ in entries}
        first_run, first_issue, _ = entries[0]
        clusters.append({
            "clusterId": cluster_id,
            "fingerprintIds": sorted({issue["fingerprint"]["id"] for _, issue, _ in entries}),
            "outcomeClass": first_issue["fingerprint"]["outcomeClass"],
            "severity": max(issue["severity"] for _, issue, _ in entries),
            "affectedApps": sorted(apps),
            "affectedRuns": sorted(run_ids),
            "reproducibility": _reproducibility(runs, apps, run_ids),
            "confidence": "discovery_only",
            "manual": any(manual for _, _, manual in entries),
            "example": {"runId": first_run["runId"], "detail": first_issue["detail"]},
        })
    return sorted(clusters, key=lambda item: (-item["severity"], -len(item["affectedApps"]), item["clusterId"]))


def _missing_apis(runs: list[dict]) -> list[dict]:
    grouped = defaultdict(list)
    for run in runs:
        for issue in run["issues"]:
            if issue["kind"] in {"missing", "unimplemented"}:
                grouped[issue["detail"]].append((run, issue))
    ranked = []
    for name, entries in grouped.items():
        apps = sorted({run["app"]["bundleId"] for run, _ in entries})
        observed = [issue["kind"] == "missing" for _, issue in entries]
        reachability = "observed_call" if any(observed) else "imported_not_observed"
        score = len(apps) * 100 + len(entries) + (50 if any(observed) else 0)
        ranked.append({
            "name": name,
            "reachability": reachability,
            "affectedApps": apps,
            "observedRuns": len(entries),
            "observedCalls": None,
            "divergenceProximity": None,
            "repairCost": None,
            "score": score,
            "fingerprintIds": sorted({issue["fingerprint"]["id"] for _, issue in entries}),
        })
    return sorted(ranked, key=lambda item: (-item["score"], item["name"]))


def build_bundle(runs: list[dict], artifacts: list[dict] | None = None, *, overrides: dict | None = None, artifact_paths: list[Path] | None = None) -> dict:
    _apply_overrides(runs, overrides or {})
    if artifact_paths is not None:
        attached = _attach_artifacts(runs, artifact_paths)
        artifacts = (artifacts or []) + [item for item in attached if item not in (artifacts or [])]
    runs = sorted(runs, key=lambda run: run["runId"])
    provenance_complete = all(
        run["build"]["commit"] and run["build"]["dirty"] is not None
        and run["build"]["binaryMatch"] == "verified" and run["app"]["sha256"]
        and run["scenario"]["sandbox"] and run["scenario"]["traceLoss"] == 0
        for run in runs
    )
    bundle = {
        "kind": KIND,
        "schema": SCHEMA,
        "runs": runs,
        "clusters": _clusters(runs),
        "missingApis": _missing_apis(runs),
        "artifacts": artifacts or [],
        "provenanceComplete": bool(provenance_complete),
        "extensions": {},
    }
    validate_bundle(bundle)
    return bundle


def combine_bundles(bundles: list[dict], *, overrides: dict | None = None) -> dict:
    """Preserve attempts for reproducibility/flakiness analysis; reject duplicate IDs."""
    runs, artifacts = [], []
    run_ids = set()
    for bundle in bundles:
        validate_bundle(bundle)
        for run in bundle["runs"]:
            if run["runId"] in run_ids:
                raise ValueError(f"duplicate runId: {run['runId']}")
            run_ids.add(run["runId"])
            runs.append(run)
        artifacts.extend(bundle.get("artifacts") or [])
    unique_artifacts = {item["path"]: item for item in artifacts}
    return build_bundle(runs, list(unique_artifacts.values()), overrides=overrides)


def validate_bundle(bundle: dict) -> None:
    """Dependency-free validation of the contract's safety-critical invariants."""
    if not isinstance(bundle, dict) or bundle.get("kind") != KIND or bundle.get("schema") != SCHEMA:
        raise ValueError("expected an hvm-evidence/v1 bundle")
    for key in ("runs", "clusters", "missingApis", "artifacts"):
        if not isinstance(bundle.get(key), list):
            raise ValueError(f"{key} must be an array")
    run_ids = set()
    artifact_ids = {item.get("id") for item in bundle["artifacts"]}
    for run in bundle["runs"]:
        required = {"runId", "attempt", "build", "host", "app", "scenario", "outcome", "observations", "issues", "artifacts"}
        if not required <= run.keys():
            raise ValueError(f"run is missing fields: {sorted(required - run.keys())}")
        if run["runId"] in run_ids:
            raise ValueError(f"duplicate runId: {run['runId']}")
        run_ids.add(run["runId"])
        if not isinstance(run["attempt"], int) or run["attempt"] < 1:
            raise ValueError("attempt must be a positive integer")
        if run["outcome"]["status"] not in {"pass", "warn", "fail", "inconclusive", "infrastructure_error"}:
            raise ValueError("invalid outcome status")
        for issue in run["issues"]:
            fp = issue.get("fingerprint") or {}
            if not re.fullmatch(r"sha256:[0-9a-f]{64}", str(fp.get("id") or "")):
                raise ValueError("invalid fingerprint ID")
            if fp.get("version") != 1 or not fp.get("outcomeClass"):
                raise ValueError("invalid fingerprint version or outcome class")
        for artifact_id in run["artifacts"]:
            if artifact_id not in artifact_ids:
                raise ValueError(f"run references unknown artifact: {artifact_id}")
    for artifact in bundle["artifacts"]:
        if artifact.get("privacy") != "private" or not re.fullmatch(r"[0-9a-f]{64}", str(artifact.get("sha256") or "")):
            raise ValueError("artifact index entries must be private and SHA-256 addressed")


def render_summary(bundle: dict) -> str:
    lines = [
        "# HydrogenVM evidence summary", "",
        f"Schema: `{SCHEMA}`. Runs: {len(bundle['runs'])}. Clusters: {len(bundle['clusters'])}.",
        f"Complete provenance: **{'yes' if bundle['provenanceComplete'] else 'no'}**.", "",
        "These are discovery signals and reproduction hypotheses, not proof that games work.", "",
        "## Failure clusters", "",
        "| Severity | Class | Games | Reproducibility | Cluster | Example |",
        "|---:|---|---:|---|---|---|",
    ]
    for cluster in bundle["clusters"]:
        example = cluster["example"]["detail"].replace("|", "/").replace("\n", " ")[:180]
        lines.append(f"| {cluster['severity']} | {cluster['outcomeClass']} | {len(cluster['affectedApps'])} | {cluster['reproducibility']['status']} | `{cluster['clusterId']}` | {example} |")
    lines += ["", "## Missing APIs (separate ranking)", "", "| Score | Reachability | Games | Observed runs | API |", "|---:|---|---:|---:|---|"]
    for api in bundle["missingApis"]:
        lines.append(f"| {api['score']} | {api['reachability']} | {len(api['affectedApps'])} | {api['observedRuns']} | `{api['name'].replace('|', '/')}` |")
    return "\n".join(lines) + "\n"


def write_bundle(bundle: dict, out: Path) -> None:
    validate_bundle(bundle)
    out.mkdir(parents=True, exist_ok=True)
    evidence = out / "evidence.json"
    if evidence.exists():
        raise FileExistsError(f"refusing to overwrite previous attempts: {evidence}")
    outputs = {
        evidence: json.dumps(bundle, ensure_ascii=False, indent=1) + "\n",
        out / "artifact-index.json": json.dumps(bundle["artifacts"], ensure_ascii=False, indent=1) + "\n",
        out / "summary.md": render_summary(bundle),
    }
    for path, text in outputs.items():
        temporary = path.with_suffix(path.suffix + ".tmp")
        temporary.write_text(text, encoding="utf-8")
        temporary.replace(path)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--probe-summary", type=Path)
    source.add_argument("--device-report", type=Path)
    parser.add_argument("--out", type=Path, required=True, help="new output directory; previous evidence is never overwritten")
    parser.add_argument("--overrides", type=Path, help="JSON with merge {fingerprint: name} and split {runId:fingerprint: name}")
    args = parser.parse_args()
    try:
        source_path = args.probe_summary or args.device_report
        report = _load_json(source_path)
        bundle = bundle_from_probe(report) if args.probe_summary else bundle_from_device(report)
        if args.overrides:
            bundle = build_bundle(bundle["runs"], bundle["artifacts"], overrides=_load_json(args.overrides))
        source_artifact = index_artifacts([source_path])
        bundle = build_bundle(bundle["runs"], bundle["artifacts"] + source_artifact)
        for run in bundle["runs"]:
            run["artifacts"] = sorted(set(run["artifacts"] + [source_artifact[0]["id"]]))
        write_bundle(bundle, args.out)
    except (OSError, ValueError, json.JSONDecodeError) as error:
        sys.exit(f"hvm-evidence: {error}")
    print(f"{len(bundle['runs'])} runs, {len(bundle['clusters'])} clusters, {len(bundle['missingApis'])} missing APIs -> {args.out / 'summary.md'}")


if __name__ == "__main__":
    main()
