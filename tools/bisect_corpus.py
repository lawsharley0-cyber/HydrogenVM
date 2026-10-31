#!/usr/bin/env python3
"""Bisect a compatibility regression in a disposable worktree.

Default runner builds and runs the compatibility harness. --command replaces
that pipeline with an argv (no shell) that must write a schema-valid harness
report to {report}. Tokens may use {repo}, {build}, {out}, {report}.
Exit 0: unique culprit found; 2: invalid endpoints/infrastructure/ambiguity.
No caller checkout, index or bisect session is modified.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from compat_harness.diffing import diff_reports
from compat_harness.schema_check import validate_report


class GateError(RuntimeError):
    pass


def run(argv, cwd, **kwargs):
    return subprocess.run([str(a) for a in argv], cwd=cwd, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT, **kwargs)


def git(repo, *args):
    result = run(["git", *args], repo)
    if result.returncode:
        raise GateError(result.stdout.strip())
    return result.stdout.strip()


def load_report(path):
    report = json.loads(Path(path).read_text(encoding="utf-8"))
    errors = validate_report(report)
    if errors:
        raise GateError("invalid harness report: " + "; ".join(errors[:5]))
    return report


def gate(repo, config):
    sha = git(repo, "rev-parse", "HEAD")
    # Fresh outputs/build per invocation: a prior commit's report cannot pass.
    with tempfile.TemporaryDirectory(prefix="hvm-corpus-") as temp:
        out = Path(temp)
        values = dict(repo=str(repo), build=str(out / "build"), out=str(out), report=str(out / "report.json"))
        command = config.get("command")
        commands = [command] if command else [
            ["cmake", "-S", "{repo}", "-B", "{build}", "-G", "Ninja", "-DCMAKE_CXX_COMPILER=clang++", "-DCMAKE_BUILD_TYPE=Release"],
            ["cmake", "--build", "{build}", "--target", "hvm", "hvm_tests", "hvm_decodecheck", "hvm_heap_tests"],
            ["bash", "tools/build_tests.sh", "{build}/tests"],
            [sys.executable, "tools/hvm_compat.py", "run", "--build-dir", "{build}", "--out", "{out}", "--require-resolved"],
        ]
        logs, rc = [], 0
        for index, cmd in enumerate(commands):
            argv = []
            for arg in cmd:
                for name, value in values.items():
                    arg = arg.replace("{" + name + "}", value)
                argv.append(arg)
            try:
                result = run(argv, repo, timeout=config["timeout"])
            except (OSError, subprocess.TimeoutExpired) as exc:
                raise GateError(f"runner could not execute: {exc}") from exc
            logs.append(result.stdout)
            rc = result.returncode
            if index < len(commands) - 1 and rc:
                raise GateError(f"preparation failed ({rc}): {result.stdout[-2000:]}")
        dest = Path(config["out"]) / sha
        dest.mkdir(parents=True, exist_ok=True)
        (dest / "run.log").write_text("\n".join(logs), encoding="utf-8")
        if rc not in (0, 1):
            raise GateError(f"runner failed with infrastructure exit {rc}; see {dest / 'run.log'}")
        report = load_report(out / "report.json")
        (dest / "report.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        if report["summary"].get("resolveErrors", 0):
            raise GateError("required corpus fixtures could not be resolved")
        baseline = load_report(config["baseline"])
        if report["host"]["pageSize"] != baseline["host"]["pageSize"]:
            raise GateError("baseline and candidate host page sizes differ")
        for suite in ("unit", "python"):
            expected = {c["id"] for c in baseline["suites"][suite]["cases"]}
            measured = {c["id"] for c in report["suites"][suite]["cases"]}
            if expected - measured:
                raise GateError(f"corpus suite {suite} omitted baseline cases")
        expected_apps = {a["id"] for a in baseline["suites"]["apps"]["apps"] if a["status"] not in ("skipped", "error")}
        measured_apps = {a["id"] for a in report["suites"]["apps"]["apps"] if a["status"] not in ("skipped", "error")}
        if expected_apps - measured_apps:
            raise GateError("corpus omitted previously measured apps")
        diff = diff_reports(baseline, report)
        (dest / "diff.json").write_text(json.dumps(diff, indent=2) + "\n", encoding="utf-8")
        # summary.ok matters even if a failed case was absent from the baseline.
        bad = not report["summary"]["ok"] or not diff["ok"]
        if rc == 1 and not bad:
            raise GateError("runner exit 1 contradicts passing report and baseline diff")
        print(f"{sha}: {'bad' if bad else 'good'}", flush=True)
        return int(bad)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", type=Path, default=Path.cwd())
    ap.add_argument("--good")
    ap.add_argument("--bad")
    ap.add_argument("--baseline", type=Path)
    ap.add_argument("--out", type=Path, default=Path("build/corpus-bisect"))
    ap.add_argument("--timeout", type=float, default=1800)
    ap.add_argument("--gate-config", type=Path, help=argparse.SUPPRESS)
    ap.add_argument("--command", nargs=argparse.REMAINDER)
    args = ap.parse_args(argv)
    try:
        if args.gate_config:
            try:
                return gate(args.repo.resolve(), json.loads(args.gate_config.read_text()))
            except Exception as exc:
                print(f"ERROR: corpus gate unavailable: {exc}", file=sys.stderr)
                return 128  # git bisect run must abort, never call this bad/skip.
        if not args.good or not args.bad or not args.baseline or args.timeout <= 0:
            raise GateError("--good, --bad, --baseline and a positive --timeout are required")
        repo, out = args.repo.resolve(), args.out.resolve()
        baseline = args.baseline.resolve()
        load_report(baseline)
        good = git(repo, "rev-parse", "--verify", args.good + "^{commit}")
        bad = git(repo, "rev-parse", "--verify", args.bad + "^{commit}")
        git(repo, "merge-base", "--is-ancestor", good, bad)
        if good == bad:
            raise GateError("good and bad endpoints must differ")
        out.mkdir(parents=True, exist_ok=True)
        result_path = out / "result.json"
        result_path.unlink(missing_ok=True)
        config = dict(command=args.command, timeout=args.timeout, baseline=str(baseline), out=str(out))
        config_path = out / "gate-config.json"
        config_path.write_text(json.dumps(config), encoding="utf-8")
        with tempfile.TemporaryDirectory(prefix="hvm-bisect-") as temp:
            checkout = Path(temp) / "checkout"
            git(repo, "-c", "core.autocrlf=false", "worktree", "add", "--detach", str(checkout), good)
            try:
                if gate(checkout, config) != 0:
                    raise GateError("good endpoint fails the corpus gate")
                git(checkout, "checkout", "--detach", bad)
                if gate(checkout, config) != 1:
                    raise GateError("bad endpoint passes the corpus gate")
                git(checkout, "bisect", "start", bad, good)
                result = run(["git", "bisect", "run", sys.executable, str(Path(__file__).resolve()),
                              "--repo", str(checkout), "--gate-config", str(config_path)], checkout)
                (out / "bisect.log").write_text(result.stdout, encoding="utf-8")
                if result.returncode:
                    raise GateError(f"bisect failed/aborted; see {out / 'bisect.log'}\n{result.stdout[-2000:]}")
                culprit = git(checkout, "rev-parse", "refs/bisect/bad")
                # Do not parse git's prose (it varies by version/config). Bisect is finished and unique when the only
                # commit reachable from the bad ref but not from any good ref is the culprit itself.
                good_refs = [r for r in git(checkout, "for-each-ref", "--format=%(refname)", "refs/bisect/good-*").split() if r]
                remaining = git(checkout, "rev-list", "refs/bisect/bad", "--not", *good_refs, good).split()
                if remaining != [culprit]:
                    raise GateError("bisect did not identify a unique first bad commit\n" + result.stdout[-2000:])
                result_path.write_text(json.dumps({"schemaVersion": 1, "firstBadCommit": culprit,
                                                  "good": good, "bad": bad}, indent=2) + "\n")
                print(f"FIRST BAD: {culprit}\n{result_path}")
            finally:
                git(repo, "worktree", "remove", "--force", str(checkout))
        return 0
    except (GateError, OSError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
