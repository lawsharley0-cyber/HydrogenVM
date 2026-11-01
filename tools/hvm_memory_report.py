#!/usr/bin/env python3
"""Summarize memory JSONL or an exported iPhone crash/session report.

No inferred zeros: unsupported counters remain absent from the summary.
Rates are endpoint deltas over the captured interval, not leak verdicts.
"""
import argparse
import datetime
import json
from pathlib import Path


def summarize(samples):
    rows = []
    for sample in samples:
        row = dict(sample)
        if "elapsedSeconds" in row:
            row["time"] = float(row["elapsedSeconds"])
        elif "utc" in row:
            row["time"] = datetime.datetime.fromisoformat(
                row["utc"].replace("Z", "+00:00")).timestamp()
        else:
            continue
        rows.append(row)
    if not rows:
        raise ValueError("no timestamped memory samples")
    if any(b["time"] < a["time"] for a, b in zip(rows, rows[1:])):
        raise ValueError("samples go backwards in time; split separate sessions")
    counters = sorted({k for row in rows for k, v in row.items()
                       if (k.endswith("Bytes") or k.endswith("Count"))
                       and isinstance(v, (int, float)) and not isinstance(v, bool)})
    result = {"samples": len(rows), "durationSeconds": rows[-1]["time"] - rows[0]["time"],
              "rateMeaning": "endpoint delta over captured interval; not proof of a leak",
              "counters": {}}
    for key in counters:
        points = [(r["time"], r[key]) for r in rows
                  if isinstance(r.get(key), (int, float)) and not isinstance(r[key], bool)]
        duration = points[-1][0] - points[0][0]
        first, last = points[0][1], points[-1][1]
        result["counters"][key] = {
            "samples": len(points), "first": first, "last": last,
            "minimum": min(v for _, v in points), "peak": max(v for _, v in points),
            "delta": last - first,
            "perSecond": (last - first) / duration if duration > 0 else None}
    return result


def read_samples(raw):
    try:
        document = json.loads(raw)
    except json.JSONDecodeError:
        lines = raw.splitlines()
        if any("[memory] " in line for line in lines):
            return [json.loads(line.split("[memory] ", 1)[1]) for line in lines if "[memory] " in line]
        return [json.loads(line) for line in lines if line.strip()]
    if isinstance(document, list):
        return document
    if not isinstance(document, dict):
        raise ValueError("expected a memory array or report object")
    report_keys = {"runtimeMemorySamples", "crashBundle", "logs", "memorySamples"}
    if not report_keys & document.keys() and ("elapsedSeconds" in document or "utc" in document):
        return [document]  # a single timestamped sample
    if document.get("runtimeMemorySamples"):
        return document["runtimeMemorySamples"]
    # iPhone shell overwrites memorySamples with its available-memory history,
    # but preserves runtimeMemorySamples in core crash bundles and logs after kills.
    crash = document.get("crashBundle")
    if isinstance(crash, dict) and crash.get("runtimeMemorySamples"):
        return crash["runtimeMemorySamples"]
    logged = [json.loads(line.split("[memory] ", 1)[1])
              for line in document.get("logs", []) if isinstance(line, str) and "[memory] " in line]
    return logged or document.get("memorySamples", [])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    raw = args.input.read_text(encoding="utf-8")
    output = json.dumps(summarize(read_samples(raw)), indent=2) + "\n"
    if args.out:
        args.out.write_text(output)
    else:
        print(output, end="")


if __name__ == "__main__":
    main()
