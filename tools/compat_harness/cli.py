"""Command line for the compatibility harness."""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import diffing, history, issues, runner
from .manifest import (
    ManifestError,
    default_manifest_path,
    legacy_allowed,
    load_manifest,
    repo_root,
    resolve_app,
)
from .schema_check import validate_diff, validate_report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="hvm_compat", description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    def add_common(p: argparse.ArgumentParser) -> None:
        p.add_argument("--library", help="owner library JSON; missing fixtures are explicitly optional")
        p.add_argument("--launches", type=int, default=1, help="launches sharing one sandbox; >1 uses graceful shutdown")
        p.add_argument("--keep-going", action="store_true", help="also collect runtime misses in an isolated sandbox")
        p.add_argument("--manifest", default=default_manifest_path())
        p.add_argument("--fixture-root", default=os.environ.get("HVM_FIXTURE_ROOT") or None)
        p.add_argument("--allow-legacy-repo", action="store_true", default=False)
        p.add_argument("--no-legacy-repo", action="store_true")
        p.add_argument("--build-dir", default=os.path.join(repo_root(), "build"))
        p.add_argument("--out", default=os.path.join(repo_root(), "build", "compat-report"))
        p.add_argument("--seconds", type=float, default=None)
        p.add_argument("--only", help="comma-separated app ids")
        p.add_argument("--hvm", default=None)
        p.add_argument("--require-resolved", action="store_true",
                       help="accepted for compatibility; a missing required fixture already fails the run")
        p.add_argument("--baseline", help="diff the new report against this JSON file")
        p.add_argument("--fuzz", type=int, default=0, help="decoder fuzz samples per mode; 0 skips")

    p_run = sub.add_parser("run", help="unit tests, Python tests, and manifest apps")
    add_common(p_run)
    p_run.add_argument("--skip-unit", action="store_true")
    p_run.add_argument("--skip-python", action="store_true")
    p_run.add_argument("--skip-apps", action="store_true")
    p_run.add_argument("--record", action="store_true",
                       help="copy the report into docs/dev-system/reports and update the index")
    p_run.add_argument("--diff-previous", action="store_true",
                       help="also diff against the previous indexed report; a missing previous report does not fail the run")

    p_unit = sub.add_parser("unit", help="existing C++ and Python suites only")
    add_common(p_unit)

    p_apps = sub.add_parser("apps", help="manifest apps only")
    add_common(p_apps)

    p_diff = sub.add_parser("diff", help="compare two reports, or a report with the previous indexed run")
    p_diff.add_argument("--previous", action="store_true",
                        help="diff the given report against the previous row in docs/dev-system/reports/index.json")
    p_diff.add_argument("--repo", default=None, help="repository root used to find the report index")
    p_diff.add_argument("baseline", nargs="?")
    p_diff.add_argument("current", nargs="?")
    p_diff.add_argument("--out")

    p_record = sub.add_parser("record", help="copy a report into docs/dev-system/reports and update the index")
    p_record.add_argument("report")
    p_record.add_argument("--repo", default=None, help="repository root; default is this checkout")

    p_issues = sub.add_parser("issues", help="emit issue markdown (dry-run unless --write)")
    p_issues.add_argument("report")
    p_issues.add_argument("--write", help="directory for markdown files; omit for dry-run")

    p_validate = sub.add_parser("validate", help="check a report against the harness schema")
    p_validate.add_argument("report")

    p_doctor = sub.add_parser("doctor", help="read-only documentation commit and evidence consistency check")
    p_doctor.add_argument("--repo", default=None)
    p_doctor.add_argument("--report", help="optional clean compatibility report with explicit backlog IDs in case IDs")

    args = parser.parse_args(argv)
    try:
        if args.cmd == "doctor":
            from pathlib import Path
            from .doctor import inspect
            report = _load(args.report) if args.report else None
            if report is not None:
                errors = validate_report(report)
                if errors:
                    for error in errors:
                        print(error, file=sys.stderr)
                    return 2
            findings = inspect(Path(args.repo or repo_root()), report)
            for finding in findings:
                location = finding.get("file", "report")
                if "line" in finding:
                    location += ":" + str(finding["line"])
                print(f"{location}: {finding['kind']}: {finding['detail']}")
            if not findings:
                print("Documentation checks passed. No task status was changed.")
            return 1 if findings else 0
        if args.cmd == "diff":
            return _cmd_diff(args)
        if args.cmd == "record":
            return _cmd_record(args)
        if args.cmd == "issues":
            return _cmd_issues(args)
        if args.cmd == "validate":
            return _cmd_validate(args)
        return _cmd_run(args)
    except ManifestError as e:
        print(f"manifest: {e}", file=sys.stderr)
        return 2
    except history.HistoryError as e:
        print(f"history: {e}", file=sys.stderr)
        return 2
    except OSError as e:
        print(f"hvm_compat: {e}", file=sys.stderr)
        return 2


def _cmd_run(args: argparse.Namespace) -> int:
    root = repo_root()
    if args.launches < 1:
        print("--launches must be positive", file=sys.stderr)
        return 2
    from .library import load_library
    manifest = load_library(args.library, args.fixture_root) if args.library else load_manifest(args.manifest)
    if args.library:
        args.manifest = args.library
        if args.seconds is None: args.seconds = 30
    allow = False if args.no_legacy_repo else True if args.allow_legacy_repo else legacy_allowed(None)
    selected = None
    if args.only:
        selected = {part.strip() for part in args.only.split(",") if part.strip()}
        known = {app["id"] for app in manifest["apps"]}
        unknown = selected - known
        if unknown:
            print("unknown app id(s): " + ", ".join(sorted(unknown)), file=sys.stderr)
            return 2
    resolved = []
    for app in manifest["apps"]:
        if selected is not None and app["id"] not in selected:
            continue
        resolved.append(resolve_app(app, repo=root, fixture_root=args.fixture_root, allow_legacy=allow))
    missing = [r for r in resolved if r["status"] != "resolved" and not r["optional"]]
    if missing:
        for item in missing:
            print(f"unresolved {item['id']}: {item['reason']}", file=sys.stderr)
        # Still write a report, then fail. Optional skips are not in this list.
    hvm = args.hvm or os.path.join(args.build_dir, "hvm")
    os.makedirs(args.out, exist_ok=True)
    if args.cmd == "unit" or (args.cmd == "run" and not args.skip_unit):
        unit = runner.run_unit(root, args.build_dir)
    else:
        unit = {"status": "skip", "passed": 0, "failed": 0, "cases": []}
    if args.cmd == "unit" or (args.cmd == "run" and not args.skip_python):
        python = runner.run_python(root)
    else:
        python = {"status": "skip", "passed": 0, "failed": 0, "cases": []}
    if args.cmd == "run":
        fuzz = runner.run_fuzz(root, args.build_dir, args.fuzz)
    else:
        fuzz = {"status": "not-run", "cases": []}
    if args.cmd == "apps" or (args.cmd == "run" and not args.skip_apps):
        apps = runner.run_apps(manifest, resolved, hvm=hvm, out_dir=args.out, seconds=args.seconds, keep_going=args.keep_going, launches=args.launches)
    else:
        apps = {"status": "skip", "apps": []}
    if args.cmd == "unit":
        apps = {"status": "skip", "apps": []}
    report = runner.build_report(
        root=root,
        unit=unit,
        python=python,
        fuzz=fuzz,
        apps=apps,
        manifest_path=args.manifest,
        fixture_root=args.fixture_root,
        allow_legacy=allow,
    )
    unresolved = [a for a in apps.get("apps") or [] if a["status"] in ("skipped", "error") and not a.get("optional")]
    if unresolved:
        report["summary"]["ok"] = False
    schema_errors = validate_report(report)
    if schema_errors:
        report["summary"]["ok"] = False
        report["schemaErrors"] = schema_errors
    report_path = os.path.join(args.out, "report.json")
    _dump(report_path, report)
    print(report_path)
    _print_summary(report)
    exit_code = 0 if report["summary"]["ok"] else 1
    if args.baseline:
        baseline = _load(args.baseline)
        diff = diffing.diff_reports(baseline, report)
        diff_errors = validate_diff(diff)
        if diff_errors:
            diff["ok"] = False
            diff["schemaErrors"] = diff_errors
        diff_path = os.path.join(args.out, "diff.json")
        _dump(diff_path, diff)
        print(diff_path)
        print(_diff_count_line("diff ", diff))
        for item in diff["regressions"]:
            print(f"  regression {item['kind']} {item.get('id', '')}: {item['message']}")
        if not diff["ok"]:
            exit_code = 1
    if getattr(args, "diff_previous", False) or getattr(args, "record", False):
        indexed = history.index_run(
            report,
            root,
            record=bool(getattr(args, "record", False)),
            diff_previous=bool(getattr(args, "diff_previous", False)),
            current_path=report_path,
        )
        prev_diff = indexed.get("previousDiff")
        if getattr(args, "diff_previous", False):
            if indexed.get("previousMissing"):
                print("no previous indexed report")
            elif prev_diff is not None:
                prev_errors = validate_diff(prev_diff)
                if prev_errors:
                    prev_diff["ok"] = False
                    prev_diff["schemaErrors"] = prev_errors
                prev_path = os.path.join(args.out, "diff-previous.json")
                _dump(prev_path, prev_diff)
                print(prev_path)
                print(_diff_count_line("previous ", prev_diff))
                for item in prev_diff["regressions"]:
                    print(f"  previous regression {item['kind']} {item.get('id', '')}: {item['message']}")
                if not prev_diff["ok"]:
                    exit_code = 1
        recorded = indexed.get("recorded")
        if recorded:
            print(os.path.join(root, "docs", "dev-system", "reports", recorded["path"]))
    if schema_errors:
        for err in schema_errors:
            print(f"schema: {err}", file=sys.stderr)
    return exit_code


def _cmd_diff(args: argparse.Namespace) -> int:
    if args.previous:
        if not args.baseline or args.current:
            print("diff --previous expects one report path", file=sys.stderr)
            return 2
        current_path = args.baseline
        repo = args.repo or repo_root()
        baseline_path = history.previous_report_path(repo, current_path)
        if baseline_path is None:
            print("no previous indexed report", file=sys.stderr)
            return 2
    else:
        if not args.baseline or not args.current:
            print("diff expects BASELINE CURRENT", file=sys.stderr)
            return 2
        baseline_path = args.baseline
        current_path = args.current
    diff = diffing.diff_reports(_load(baseline_path), _load(current_path))
    errors = validate_diff(diff)
    if errors:
        diff["ok"] = False
        diff["schemaErrors"] = errors
    text = json.dumps(diff, indent=2) + "\n"
    if args.out:
        _dump(args.out, diff)
        print(args.out)
    else:
        sys.stdout.write(text)
    return 0 if diff["ok"] else 1


def _cmd_record(args: argparse.Namespace) -> int:
    repo = args.repo or repo_root()
    entry = history.record_report(_load(args.report), repo)
    print(os.path.join(repo, "docs", "dev-system", "reports", entry["path"]))
    return 0


def _cmd_issues(args: argparse.Namespace) -> int:
    report = _load(args.report)
    docs = issues.generate(report, write_dir=args.write)
    if not docs:
        print("DRY-RUN (not filed): no clustered findings")
        return 0
    if args.write:
        print(f"wrote {len(docs)} issue file(s) under {args.write} (not filed)")
        for doc in docs:
            print(doc.get("path", doc["filename"]))
        return 0
    print("DRY-RUN (not filed): issue markdown follows. Pass --write DIR to save files.")
    for doc in docs:
        print(f"\n----- {doc['filename']} -----\n")
        sys.stdout.write(doc["body"])
        if not doc["body"].endswith("\n"):
            print()
    return 0


def _cmd_validate(args: argparse.Namespace) -> int:
    errors = validate_report(_load(args.report))
    if errors:
        for err in errors:
            print(err)
        return 1
    print("ok")
    return 0


def _diff_count_line(prefix: str, diff: dict) -> str:
    return (
        f"{prefix}ok={diff['ok']} regressions={len(diff['regressions'])} "
        f"attention={len(diff['attention'])} informational={len(diff['informational'])}"
    )


def _print_summary(report: dict) -> None:
    summary = report["summary"]
    print(
        "summary "
        f"ok={summary['ok']} unitFailed={summary['unitFailed']} "
        f"pythonFailed={summary['pythonFailed']} appsCrashed={summary['appsCrashed']} "
        f"appsSkipped={summary['appsSkipped']} clusters={summary['clusterCount']}"
    )


def _load(path: str) -> dict:
    with open(path, encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict):
        raise ManifestError(f"{path} is not a JSON object")
    return data


def _dump(path: str, data: dict) -> None:
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2)
        f.write("\n")
