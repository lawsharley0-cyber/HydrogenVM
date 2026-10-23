#!/usr/bin/env python3
"""Run apps on the software rasterizer and on the host GPU backend (`hvm run --gpu`)
and compare the last frames.

    python3 tools/gpu_compare.py --hvm build/hvm --seconds 12 Tiny_Tower_v1.0_os30.ipa ...

Each input is an .ipa or .app. Fails (exit 1) when the GPU run crashes where the
software run didn't, presents no frames, or its last frame differs from the
software frame by more than --max-diff (mean absolute difference per channel,
0-255). Animated scenes drift between runs; pick apps and durations that land on
a still screen (Tiny Tower's tutorial does).
"""
import argparse, json, os, subprocess, sys, tempfile, zipfile


def app_from(path, work):
    if path.endswith(".app"):
        return path
    dest = os.path.join(work, os.path.basename(path) + ".d")
    with zipfile.ZipFile(path) as z:
        for m in z.infolist():
            target = os.path.realpath(os.path.join(dest, m.filename))
            if not target.startswith(os.path.realpath(dest) + os.sep):
                raise SystemExit(f"{path}: unsafe path {m.filename}")
        z.extractall(dest)
    payload = os.path.join(dest, "Payload")
    apps = [d for d in os.listdir(payload) if d.endswith(".app")]
    if not apps:
        raise SystemExit(f"{path}: no .app in Payload/")
    return os.path.join(payload, apps[0])


def run(hvm, app, seconds, gpu, work, tag):
    home = os.path.join(work, tag + "-home")
    shot = os.path.join(work, tag + ".png")
    report = os.path.join(work, tag + ".json")
    cmd = [hvm, "run", app, "--home", home, "--run-for", str(seconds), "--quiet", "--report", report, "--screenshot", shot]
    if gpu:
        cmd.insert(3, "--gpu")
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=seconds + 120)
    data = json.load(open(report)) if os.path.exists(report) else {}
    return data, shot if os.path.exists(shot) else None, proc


def mean_diff(a, b):
    from PIL import Image, ImageChops, ImageStat
    ia, ib = Image.open(a).convert("RGB"), Image.open(b).convert("RGB")
    if ia.size != ib.size:
        return 255.0
    stat = ImageStat.Stat(ImageChops.difference(ia, ib))
    return sum(stat.mean) / 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("inputs", nargs="+")
    ap.add_argument("--hvm", default="build/hvm")
    ap.add_argument("--seconds", type=int, default=12)
    ap.add_argument("--max-diff", type=float, default=6.0)
    ap.add_argument("--out", help="keep screenshots and reports here")
    args = ap.parse_args()
    work = args.out or tempfile.mkdtemp(prefix="gpu-compare-")
    os.makedirs(work, exist_ok=True)
    failed = False
    for inp in args.inputs:
        name = os.path.splitext(os.path.basename(inp))[0]
        app = app_from(inp, work)
        sw, sw_shot, _ = run(args.hvm, app, args.seconds, False, work, name + "-sw")
        gpu, gpu_shot, gproc = run(args.hvm, app, args.seconds, True, work, name + "-gpu")
        problems = []
        if gproc.returncode == 2 and "no host OpenGL ES" in gproc.stderr:
            print(f"skip {name}: {gproc.stderr.strip()}")
            continue
        if gpu.get("crashed") and not sw.get("crashed"):
            problems.append("GPU run crashed: " + (gpu.get("crashReport") or "").splitlines()[0:1].__str__())
        frames = (gpu.get("perf") or {}).get("framesPresented", 0)
        if not frames:
            problems.append("GPU run presented no frames")
        diff = mean_diff(sw_shot, gpu_shot) if sw_shot and gpu_shot else 255.0
        if diff > args.max_diff:
            problems.append(f"last frames differ (mean {diff:.1f} > {args.max_diff})")
        sw_ms = (sw.get("perf") or {}).get("avgFrameMs", 0)
        gpu_ms = (gpu.get("perf") or {}).get("avgFrameMs", 0)
        status = "FAIL" if problems else "ok"
        print(f"{status:4s} {name}: diff {diff:.1f}, frames sw {(sw.get('perf') or {}).get('framesPresented', 0)} "
              f"gpu {frames}, avg frame sw {sw_ms:.1f} ms gpu {gpu_ms:.1f} ms" + ("; " + "; ".join(problems) if problems else ""))
        failed |= bool(problems)
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
