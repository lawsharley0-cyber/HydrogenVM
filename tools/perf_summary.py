#!/usr/bin/env python3
"""Summarize private perf/runner/CoreMark evidence; never infer a speedup.

Raw logs, perf.data, guest profiles, screenshots and saves stay outside Git.
Usage: python3 tools/perf_summary.py --report run.json --perf host.txt
       [--coremark coremark-1.log --coremark coremark-2.log] --out baseline.json
"""
import argparse
import json
import math
from pathlib import Path
import re


def coremark(text):
    score = re.search(r"Iterations/Sec\s*:\s*([0-9.]+)", text)
    duration = re.search(r"Total time \(secs\)\s*:\s*([0-9.]+)", text)
    if "Correct operation validated" not in text or not score or not duration:
        raise ValueError("CoreMark is missing its validation or measurements")
    score, duration = float(score[1]), float(duration[1])
    if not math.isfinite(score) or score <= 0 or not math.isfinite(duration) or duration < 10:
        raise ValueError("CoreMark must be a positive, validated run of at least 10 seconds")
    return {"iterations_per_second": score, "seconds": duration}


def host_samples(text, limit=20):
    rows = []
    for line in text.splitlines():
        # perf report --stdio --no-children --sort symbol has a single overhead
        # percentage followed by the privilege marker and symbol.
        match = re.match(r"\s*([0-9]+(?:\.[0-9]+)?)%\s+(?:.*?\s+)?\[.\]\s+(.+?)\s*$", line)
        if match:
            percent = float(match[1])
            if percent > 100:
                raise ValueError("invalid perf sample percentage")
            # Newer perf versions append aligned IPC/coverage columns even when
            # only overhead,symbol were requested. Keep the demangled name,
            # including its ordinary single spaces, without those columns.
            symbol = re.split(r"\s{2,}", match[2].strip(), maxsplit=1)[0]
            rows.append({"percent": percent, "symbol": symbol})
    if not rows:
        raise ValueError("no symbol samples in perf report (profiling did not succeed)")
    return sorted(rows, key=lambda row: row["percent"], reverse=True)[:limit]


def summarize(report, perf_text, coremark_logs):
    if report.get("crashed") or report.get("crashReport") or report.get("loadError") or report.get("exitStatus", 0):
        raise ValueError("failed/crashed runs are not successful performance baselines")
    metrics = report.get("perf", {})
    required = ["wallSeconds", "framesPresented", "avgFrameMs", "p95FrameMs", "p99FrameMs", "guestMIPS"]
    for key in required:
        value = metrics.get(key)
        if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(value) or value < 0:
            raise ValueError(f"missing or invalid performance metric: {key}")
    if metrics["wallSeconds"] <= 0 or metrics["framesPresented"] <= 0:
        raise ValueError("a game baseline requires positive duration and presented frames")
    return {"schema": 1, "game_metrics": {key: metrics[key] for key in required},
            "top_host_samples": host_samples(perf_text),
            "coremark": [coremark(log) for log in coremark_logs],
            "tainted": report.get("tainted"), "stub_hits": report.get("stubHits"),
            "interpretation": "Single-build baseline only. Sampling overhead is included; no speedup or gameplay equivalence is established."}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--report", required=True, type=Path)
    parser.add_argument("--perf", required=True, type=Path)
    parser.add_argument("--coremark", action="append", type=Path, default=[])
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args()
    result = summarize(json.loads(args.report.read_text()), args.perf.read_text(), [p.read_text() for p in args.coremark])
    # Exclusive creation protects existing evidence from accidental overwrite.
    with args.out.open("x") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")


if __name__ == "__main__":
    main()
