#!/usr/bin/env python3
"""Feed timed text/keys into a running `hvm run --input-file FILE`.

    python3 tools/bench/input_feed.py FILE 34:type:Lawson 37:backspace 38:return &
    ./build/hvm run App.app ... --input-file FILE

Each argument is SECONDS:COMMAND, written to FILE at that time after this script
starts (start it right before hvm). "type:TEXT" types one character every 0.4 s.
Other commands are passed through as-is; see the --input-file reader in
tests/runner/HvmRun.cpp for the full list (text <hex>, backspace, return, ...).
"""
import sys, time

def main():
    if len(sys.argv) < 3:
        print(__doc__); return 2
    path = sys.argv[1]
    open(path, "w").close()
    t0 = time.time(); events = []
    for arg in sys.argv[2:]:
        t, cmd = arg.split(":", 1); t = float(t)
        if cmd.startswith("type:"):
            for i, ch in enumerate(cmd[5:]):
                events.append((t + i * 0.4, "text " + ch.encode().hex()))
        else:
            events.append((t, cmd))
    for t, cmd in sorted(events):
        wait = t - (time.time() - t0)
        if wait > 0: time.sleep(wait)
        with open(path, "a") as f: f.write(cmd + "\n")
    return 0

if __name__ == "__main__":
    sys.exit(main())
