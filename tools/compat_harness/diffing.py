"""Compare two harness reports and separate regressions from noise.

Hard regressions are facts that should be stable on a 4 KB Linux host:
unit or Python failures, a launch that used to succeed, a new crash, a new
or changed load error, a new load-time unimplemented import, presented frames
dropping to zero, a framebuffer that became all black or all white after
previously being varied or mostly one color, or mixer output that went from
active to silent.

Timing, MIPS, and raw audio sample counts are informational. ``frames`` and
``nonSilentFrames`` are noted when they move by 25% or more and the mixer
signal stays ``active``. A drop that stays active is not a failure. Log text
and runtime-only missing functions are attention or informational. A
three-second smoke does not execute the same guest path on every machine. A
baseline row that was skipped is new coverage when the current run is real,
not a regression.
"""
from __future__ import annotations

PERF_REL = 0.25
_BLANK = frozenset({"all-black", "all-white"})
_RICH = frozenset({"varied", "mostly-one-color"})
_MEASURED = frozenset({"ran", "crashed", "load-error", "timeout"})


def diff_reports(baseline: dict, current: dict) -> dict:
    regressions: list[dict] = []
    improvements: list[dict] = []
    attention: list[dict] = []
    informational: list[dict] = []

    base_host = (baseline.get("host") or {}).get("pageSize")
    cur_host = (current.get("host") or {}).get("pageSize")
    if base_host and cur_host and base_host != cur_host:
        informational.append({
            "kind": "host-page-size",
            "message": f"page size {base_host} -> {cur_host}; a 4 KB pass is not a 16 KB memory-model pass",
        })

    for suite in ("unit", "python"):
        _diff_cases(suite, baseline, current, regressions, improvements)

    base_apps = _apps(baseline)
    cur_apps = _apps(current)
    for ident in sorted(set(base_apps) | set(cur_apps)):
        _diff_app(ident, base_apps.get(ident), cur_apps.get(ident), regressions, improvements, attention, informational)

    return {
        "schemaVersion": 1,
        "kind": "hvm-compat-diff",
        "ok": not regressions,
        "regressions": regressions,
        "improvements": improvements,
        "attention": attention,
        "informational": informational,
    }


def _diff_cases(suite, baseline, current, regressions, improvements) -> None:
    before = {c["id"]: c for c in ((baseline.get("suites") or {}).get(suite) or {}).get("cases") or []}
    after = {c["id"]: c for c in ((current.get("suites") or {}).get(suite) or {}).get("cases") or []}
    for ident in sorted(set(before) | set(after)):
        b = before.get(ident)
        a = after.get(ident)
        b_status = (b or {}).get("status")
        a_status = (a or {}).get("status")
        if b_status == "pass" and a_status != "pass":
            regressions.append({
                "kind": "test",
                "id": f"{suite}:{ident}",
                "message": f"{suite} {ident} was pass and is now {a_status or 'missing'}",
            })
        elif b_status != "pass" and a_status == "pass":
            improvements.append({
                "kind": "test",
                "id": f"{suite}:{ident}",
                "message": f"{suite} {ident} now passes (was {b_status or 'missing'})",
            })


def _diff_app(ident, before, after, regressions, improvements, attention, informational) -> None:
    if _diff_app_status(ident, before, after, regressions, informational):
        return
    _diff_app_launch_and_crash(ident, before, after, regressions, improvements)
    _diff_app_symbols(ident, before, after, regressions, improvements, attention)
    _diff_app_media(ident, before, after, regressions, improvements, informational)


def _diff_app_status(ident, before, after, regressions, informational) -> bool:
    if before is None:
        informational.append({"kind": "new-app", "id": ident, "message": f"{ident} was not in the baseline"})
        return True
    if after is None:
        regressions.append({"kind": "missing-app", "id": ident, "message": f"{ident} is missing from the current report"})
        return True
    if after.get("status") == "skipped" and before.get("status") not in ("skipped", "error"):
        regressions.append({
            "kind": "fixture-skipped",
            "id": ident,
            "message": f"{ident} ran before and is now skipped: {after.get('reason')}",
        })
        return True
    if before.get("status") in ("skipped", "error") and after.get("status") in ("skipped", "error"):
        if before.get("status") != after.get("status") or (before.get("reason") or "") != (after.get("reason") or ""):
            informational.append({
                "kind": "fixture",
                "id": ident,
                "message": f"{ident} fixture state {before.get('status')} -> {after.get('status')}: {after.get('reason')}",
            })
        return True
    if before.get("status") == "skipped" and after.get("status") in _MEASURED:
        informational.append({
            "kind": "new-coverage",
            "id": ident,
            "message": (
                f"{ident} was skipped in the baseline; this run is new coverage "
                f"({after.get('status')}), not a regression"
            ),
        })
        return True
    return False


def _diff_app_launch_and_crash(ident, before, after, regressions, improvements) -> None:
    b_launch = bool(before.get("launchSuccess"))
    a_launch = bool(after.get("launchSuccess"))
    if b_launch and not a_launch:
        regressions.append({
            "kind": "launch",
            "id": ident,
            "message": f"{ident} launched in the baseline and did not now ({after.get('status')}: {after.get('reason') or after.get('loadError') or ''})".strip(),
        })
    elif not b_launch and a_launch:
        improvements.append({"kind": "launch", "id": ident, "message": f"{ident} now launches"})

    if not before.get("crashed") and after.get("crashed"):
        regressions.append({"kind": "crash", "id": ident, "message": f"{ident} crashes; baseline did not"})
    elif before.get("crashed") and not after.get("crashed"):
        improvements.append({"kind": "crash", "id": ident, "message": f"{ident} no longer crashes"})

    b_err = (before.get("loadError") or "").strip()
    a_err = (after.get("loadError") or "").strip()
    if a_err and a_err != b_err:
        regressions.append({"kind": "load", "id": ident, "message": a_err[:300]})
    elif b_err and not a_err:
        improvements.append({"kind": "load", "id": ident, "message": f"{ident} load error cleared"})


def _diff_app_symbols(ident, before, after, regressions, improvements, attention) -> None:
    b_imp = set(before.get("unimplementedImports") or [])
    a_imp = set(after.get("unimplementedImports") or [])
    for name in sorted(a_imp - b_imp):
        regressions.append({"kind": "unimplemented-import", "id": ident, "message": f"{ident} newly unimplemented: {name}"})
    for name in sorted(b_imp - a_imp):
        improvements.append({"kind": "unimplemented-import", "id": ident, "message": f"{ident} import now bound: {name}"})

    b_miss = set(_bare(x) for x in (before.get("missingFunctions") or []))
    a_miss = set(_bare(x) for x in (after.get("missingFunctions") or []))
    for name in sorted(a_miss - b_miss):
        attention.append({
            "kind": "runtime-miss",
            "id": ident,
            "message": f"{ident} hit a new runtime miss ({name}); 3s smokes do not always reach the same call",
        })
    for name in sorted(b_miss - a_miss):
        improvements.append({"kind": "runtime-miss", "id": ident, "message": f"{ident} no longer hits {name}"})


def _diff_app_media(ident, before, after, regressions, improvements, informational) -> None:
    a_launch = bool(after.get("launchSuccess"))
    b_frames = ((before.get("perf") or {}).get("framesPresented")) or 0
    a_frames = ((after.get("perf") or {}).get("framesPresented")) or 0
    if b_frames and not a_frames and a_launch:
        regressions.append({"kind": "frames", "id": ident, "message": f"{ident} presented frames dropped from {b_frames} to 0"})
    b_signal = (before.get("frame") or {}).get("signal")
    a_signal = (after.get("frame") or {}).get("signal")
    if b_signal in _RICH and a_signal in _BLANK:
        regressions.append({
            "kind": "render",
            "id": ident,
            "message": f"{ident} framebuffer went from {b_signal} to {a_signal}",
        })
    elif b_signal in _BLANK and a_signal in _RICH:
        improvements.append({
            "kind": "render",
            "id": ident,
            "message": f"{ident} framebuffer is now {a_signal} (was {b_signal})",
        })
    elif b_signal != a_signal and a_signal not in (None, "not-run"):
        informational.append({
            "kind": "render-signal",
            "id": ident,
            "message": f"{ident} frame signal {b_signal} -> {a_signal}",
        })

    b_audio = (before.get("audio") or {}).get("signal")
    a_audio = (after.get("audio") or {}).get("signal")
    if b_audio == "active" and a_audio == "silent":
        regressions.append({
            "kind": "audio",
            "id": ident,
            "message": f"{ident} mixer went from active to silent",
        })
    elif b_audio == "silent" and a_audio == "active":
        improvements.append({
            "kind": "audio",
            "id": ident,
            "message": f"{ident} mixer is now active (was silent)",
        })
    elif b_audio != a_audio and a_audio not in (None, "not-run", "not-measured"):
        informational.append({
            "kind": "audio-signal",
            "id": ident,
            "message": f"{ident} audio signal {b_audio} -> {a_audio}",
        })
    _audio_count_note(ident, before.get("audio") or {}, after.get("audio") or {}, informational)

    _perf_note(ident, before.get("perf") or {}, after.get("perf") or {}, informational)


def _audio_count_note(ident, before, after, informational) -> None:
    """Note mixer sample-count movement that does not change the signal.

    ``frames`` counts rendered PCM frames. ``nonSilentFrames`` counts the
    non-zero ones. Both stay informational while the signal is ``active``.
    A move of 25% or more is noted. A baseline of zero is noted when the
    current count is not zero, because there is no relative delta. Going from
    ``active`` to ``silent`` is a regression and is handled above.
    """
    if before.get("signal") != "active" or after.get("signal") != "active":
        return
    for key in ("frames", "nonSilentFrames"):
        b = before.get(key)
        a = after.get(key)
        if isinstance(b, bool) or isinstance(a, bool):
            continue
        if not isinstance(b, (int, float)) or not isinstance(a, (int, float)):
            continue
        if b == 0:
            if a == 0:
                continue
            informational.append({
                "kind": "audio-samples",
                "id": ident,
                "message": f"{ident} audio {key} {b} -> {a}; signal stayed active; not a failure",
            })
            continue
        delta = (a - b) / abs(b)
        if abs(delta) >= PERF_REL:
            informational.append({
                "kind": "audio-samples",
                "id": ident,
                "message": (
                    f"{ident} audio {key} {b} -> {a} ({delta:+.0%}); "
                    "signal stayed active; not a failure"
                ),
            })


def _perf_note(ident, before, after, informational) -> None:
    for key in ("avgFrameMs", "guestMIPS"):
        b = before.get(key)
        a = after.get(key)
        if not isinstance(b, (int, float)) or not isinstance(a, (int, float)) or b == 0:
            continue
        delta = (a - b) / abs(b)
        if abs(delta) >= PERF_REL:
            informational.append({
                "kind": "perf",
                "id": ident,
                "message": f"{ident} {key} {b} -> {a} ({delta:+.0%}); not a failure",
            })


def _apps(report: dict) -> dict:
    apps = ((report.get("suites") or {}).get("apps") or {}).get("apps") or []
    return {a["id"]: a for a in apps if isinstance(a, dict) and "id" in a}


def _bare(name: str) -> str:
    text = str(name)
    if " (" in text:
        text = text.split(" (", 1)[0]
    return text
