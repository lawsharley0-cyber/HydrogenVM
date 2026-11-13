#!/usr/bin/env python3
"""Watch an animation: grab the frames a game presents and lay them out on one image.

    python3 tools/bench/contact_sheet.py --out DIR --from 12 --to 26 -- ./build/hvm run App.app --home H --gpu ...

Adds --frame-file and --run-for to the hvm command, saves each distinct frame
presented between --from and --to seconds as DIR/fNNN_T.png and 16 of them as
DIR/sheet.png. Use it to judge animation (frozen, skipping, wrong pictures).
Needs Pillow.
"""
import argparse, glob, os, struct, subprocess, sys, threading, time

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True); ap.add_argument("--from", dest="start", type=float, default=0)
    ap.add_argument("--to", dest="end", type=float, required=True)
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    from PIL import Image, ImageChops
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    os.makedirs(a.out, exist_ok=True)
    for f in glob.glob(os.path.join(a.out, "f*.png")): os.remove(f)
    live = os.path.join(a.out, "live.hvf")
    t0 = time.time(); stop = False; n = [0]
    def grab():
        last = None
        while not stop:
            time.sleep(0.05)
            t = time.time() - t0
            if t < a.start: continue
            try:
                st = os.stat(live)
                if st.st_mtime_ns == last: continue
                last = st.st_mtime_ns
                d = open(live, "rb").read()
                _, w, h, _ = struct.unpack("<IIII", d[:16])
                Image.frombytes("RGBA", (w, h), d[16:16 + w * h * 4]).convert("RGB").save(os.path.join(a.out, f"f{n[0]:04d}_{t:.1f}.png"))
                n[0] += 1
            except Exception:
                pass
    th = threading.Thread(target=grab, daemon=True); th.start()
    rc = subprocess.call(cmd + ["--frame-file", live, "--run-for", str(a.end)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    stop = True; th.join(1)
    files = sorted(glob.glob(os.path.join(a.out, "f*.png")))
    keep = []; last = None
    for f in files:
        im = Image.open(f).convert("RGB")
        if last is None or ImageChops.difference(im, last).getbbox(): keep.append(im)
        last = im
    print(f"{len(files)} frames captured, {len(keep)} distinct")
    sel = keep[::max(1, len(keep) // 16)][:16]
    if sel:
        w, h = sel[0].size; k = 5
        sheet = Image.new("RGB", ((w // k) * 8, (h // k) * 2))
        for i, im in enumerate(sel): sheet.paste(im.resize((w // k, h // k)), ((i % 8) * (w // k), (i // 8) * (h // k)))
        sheet.save(os.path.join(a.out, "sheet.png"))
    return rc

if __name__ == "__main__":
    sys.exit(main())
