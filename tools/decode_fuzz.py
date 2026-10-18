#!/usr/bin/env python3
"""Decoder coverage fuzzer.

Generates random ARM and Thumb-2 encodings, asks llvm-mc which ones are valid
user-mode ARMv7-A (+VFPv4/NEON/idiv) instructions, runs them through
HydrogenVM's decoder, and reports valid instructions the decoder rejects,
grouped by mnemonic.

usage: tools/decode_fuzz.py <hvm_decodecheck> [count-per-mode]
"""
import collections, random, re, subprocess, sys

MC = "llvm-mc"
MATTR = "+neon,+vfp4,+hwdiv,+hwdiv-arm,+v7,+mp"
# System / privileged / not reachable from iOS user code: not our concern.
IGNORE = re.compile(
    r"^(cps|srs|rfe|eret|hvc|smc|mcrr2?|mrrc2?|cdp2?|ldc2?l?|stc2?l?|mcr2?|mrc2?|setend|"
    r"msr|mrs|dbg|sev|sevl|wfe|wfi|yield|nop|bxj|subs|movs|vmsr|vmrs|ldrex|"
    r"vcvtb|vcvtt|vldmia|vstmia|vldmdb|vstmdb|fldmiax|fldmdbx|fstmiax|fstmdbx|"
    r"udf|bkpt|svc|hint|esb|csdb|ssbb|pssbb|sb|clrbhb|dsb|dmb|isb|pld|pldw|pli)")

def gen(mode, n, rng):
    out = []
    for _ in range(n):
        if mode == "a":
            w = rng.getrandbits(32)
            if (w >> 28) == 0xF and rng.random() < 0.7:   # bias toward conditional space
                w = (w & 0x0FFFFFFF) | (rng.randrange(15) << 28)
            out.append(w)
        else:
            hw1 = rng.getrandbits(16)
            if rng.random() < 0.7:
                hw1 = 0xE800 | (hw1 & 0x17FF)              # force a 32-bit encoding
            out.append((hw1 << 16) | rng.getrandbits(16))
    return out

def valid(mode, words):
    """Return {word: disassembly} for words llvm-mc decodes cleanly as one instruction.

    Uses --show-encoding so each output instruction carries its exact bytes,
    and drops any input line llvm-mc warned about (invalid or UNPREDICTABLE)."""
    triple = "armv7" if mode == "a" else "thumbv7"
    res = {}
    chunk = 4000
    enc_re = re.compile(r"^\s*(.*?)\s*@ encoding: \[(.*)\]")
    for i in range(0, len(words), chunk):
        part = words[i:i + chunk]
        lines, want = [], []
        for w in part:
            if mode == "a":
                b = w.to_bytes(4, "little")
            else:
                hw1, hw2 = w >> 16, w & 0xFFFF
                b = hw1.to_bytes(2, "little") + (hw2.to_bytes(2, "little") if (hw1 >> 11) >= 0x1D else b"")
            want.append(b)
            lines.append(" ".join(f"0x{x:02x}" for x in b))
        p = subprocess.run([MC, f"-triple={triple}", f"-mattr={MATTR}", "--disassemble", "--show-encoding"],
                           input="\n".join(lines) + "\n", capture_output=True, text=True)
        warned = set(int(m.group(1)) for m in re.finditer(r"<stdin>:(\d+):\d+: warning", p.stderr))
        decoded = {}
        for l in p.stdout.splitlines():
            m = enc_re.match(l)
            if not m:
                continue
            bs = bytes(int(x, 16) for x in m.group(2).split(","))
            decoded[bs] = m.group(1).replace("\t", " ")
        for j, (w, b) in enumerate(zip(part, want)):
            if (j + 1) not in warned and b in decoded:
                res[w] = decoded[b]
    return res

def main():
    tool = sys.argv[1]
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 40000
    rng = random.Random(1234)
    report = collections.Counter()
    examples = {}
    totals = {}
    for mode in ("a", "t"):
        words = gen(mode, n, rng)
        ok = valid(mode, words)
        ok = {w: s for w, s in ok.items() if not IGNORE.match(s.split()[0].split(".")[0])}
        # LDM/STM with PC as the base register is UNPREDICTABLE (llvm-mc doesn't flag it).
        ok = {w: s for w, s in ok.items() if not re.match(r"^(ldm|stm)\S*\s+pc!?,", s)}
        # NEON element/structure transfers with PC as base, and fixed-point VCVT with
        # negative fraction bits, are UNPREDICTABLE too.
        ok = {w: s for w, s in ok.items() if not re.search(r"\[pc(:\d+)?\]", s) and not re.search(r"#-\d+$", s)}
        totals[mode] = len(ok)
        inp = "\n".join(f"{mode} {w:08x}" for w in ok) + "\n"
        p = subprocess.run([tool], input=inp, capture_output=True, text=True)
        for line in p.stdout.split("\n"):
            if not line.strip():
                continue
            w = int(line.split()[1], 16)
            mn = ok[w].split()[0]
            report[(mode, mn)] += 1
            examples.setdefault((mode, mn), (w, ok[w]))
    rejected = sum(report.values())
    print(f"valid user-mode encodings tested: ARM {totals['a']}, Thumb {totals['t']}; rejected by decoder: {rejected}")
    for (mode, mn), cnt in report.most_common():
        w, text = examples[(mode, mn)]
        print(f"  {'ARM  ' if mode == 'a' else 'Thumb'} {mn:14s} x{cnt:<5d} e.g. {w:08x}  {text}")

if __name__ == "__main__":
    main()
