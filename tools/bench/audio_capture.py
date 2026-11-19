#!/usr/bin/env python3
"""Run `hvm run ...` and measure what it plays.

    python3 tools/bench/audio_capture.py [--log FILE] [--wav FILE] -- ./build/hvm run App.app --home H --run-for 40 ...

hvm writes its mixed output (16-bit little-endian stereo PCM datagrams) to the
socket given with --audio-fd; this script appends that option itself. It prints
the stretches of time (seconds from start) during which the output was audible,
which is how to check "does this game have sound effects / music / voice".
"""
import argparse, os, socket, struct, subprocess, sys, time, wave

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--log", help="where hvm's own output goes (default: discarded)")
    ap.add_argument("--wav", help="also save everything played as a WAV file (44.1 kHz stereo)")
    ap.add_argument("--threshold", type=int, default=300, help="peak sample counted as audible")
    ap.add_argument("cmd", nargs=argparse.REMAINDER)
    a = ap.parse_args()
    cmd = a.cmd[1:] if a.cmd and a.cmd[0] == "--" else a.cmd
    if not cmd:
        ap.error("give the hvm command after --")
    rx, tx = socket.socketpair(socket.AF_UNIX, socket.SOCK_DGRAM)
    rx.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1 << 24)
    out = open(a.log, "w") if a.log else subprocess.DEVNULL
    p = subprocess.Popen(cmd + ["--audio-fd", str(tx.fileno())], pass_fds=[tx.fileno()], stdout=out, stderr=subprocess.STDOUT)
    tx.close()
    rx.settimeout(0.5)
    wav = None
    if a.wav:
        wav = wave.open(a.wav, "wb"); wav.setnchannels(2); wav.setsampwidth(2); wav.setframerate(44100)
    t0 = time.time(); loud = []; total = 0
    while p.poll() is None:
        try:
            d = rx.recv(1 << 16)
        except socket.timeout:
            continue
        except OSError:
            break
        total += len(d)
        if wav: wav.writeframes(d)
        n = len(d) // 2
        if n > 8:
            v = struct.unpack("<%dh" % n, d[:n * 2])
            if max(abs(x) for x in v[8:]) > a.threshold:
                loud.append(time.time() - t0)
    if wav: wav.close()
    spans = []
    for t in loud:
        if spans and t - spans[-1][1] < 0.25: spans[-1][1] = t
        else: spans.append([t, t])
    print(f"received {total} bytes of audio")
    print("audible:", ", ".join(f"{s:.1f}-{e:.1f}s" for s, e in spans if e - s > 0.05) or "never")
    return p.returncode or 0

if __name__ == "__main__":
    sys.exit(main())
