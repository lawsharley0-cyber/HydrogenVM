#!/usr/bin/env python3
"""Differential CPU check: HydrogenVM's interpreter against Unicorn (an independent ARM emulator
built from QEMU).

Every case is one to three ARMv7 instructions (A32 or Thumb-2) with a random starting state.
Both emulators run it from the same registers, flags, VFP/NEON registers and a 4 KB data window;
any difference in the result is a finding. Instructions Unicorn rejects, or that leave the
sequence (branches, PC writes), are skipped. Cases HydrogenVM reports as undefined while Unicorn
runs them are listed separately as unimplemented instructions.

    python3 tools/cpudiff/cpudiff.py --cases 20000 --seed 1
    python3 tools/cpudiff/cpudiff.py --corpus path/to/libgame.so    # also use real game code

Exit status: 0 no new mismatches, 1 new mismatches (see --known), 2 setup problem.
Needs `pip install unicorn` (CI installs it); the HydrogenVM side is build/hvm_cpudiff.
"""
import argparse
import json
import random
import shutil
import struct
import subprocess
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CODE, SLOT = 0x00100000, 64
WINDOW, WINDOW_SIZE = 0x10000000, 0x1000
SP = WINDOW + 0x800
FPSCR_FLAGS = 0x9F          # cumulative exception bits (IOC DZC OFC UFC IXC IDC)


def pattern() -> bytes:
    return bytes((i * 131 + 7) & 0xFF for i in range(WINDOW_SIZE))


def fnv(data: bytes) -> int:
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


# ------------------------------------------------------------------ generation ---

def arm_word(rng: random.Random) -> int:
    cond = 0xE if rng.random() < 0.9 else rng.randrange(0xF)
    kind = rng.random()
    if kind < 0.30:                                   # data processing, multiply, extra load/store
        w = (cond << 28) | rng.getrandbits(26)
    elif kind < 0.45:                                 # media: parallel add/sub, saturate, extend, bitfield
        w = (cond << 28) | 0x06000010 | (rng.getrandbits(25) & 0x01FFFFEF)
    elif kind < 0.60:                                 # load/store word/byte
        w = (cond << 28) | 0x04000000 | rng.getrandbits(25)
    elif kind < 0.65:                                 # load/store multiple
        w = (cond << 28) | 0x08000000 | rng.getrandbits(25)
    elif kind < 0.78:                                 # VFP data processing
        w = (cond << 28) | 0x0E000A00 | (rng.getrandbits(24) & 0x00FF01EF) | (rng.getrandbits(1) << 8)
    elif kind < 0.82:                                 # VFP load/store, transfers
        w = (cond << 28) | rng.choice([0x0D000A00, 0x0C000A00, 0x0E000A10]) | (rng.getrandbits(24) & 0x00FF01FF)
    elif kind < 0.97:                                 # NEON data processing (unconditional)
        w = 0xF2000000 | rng.getrandbits(25)
    else:                                             # NEON element/structure load/store
        w = 0xF4000000 | (rng.getrandbits(24) & 0x00EFFFFF)
    return w


def thumb_insn(rng: random.Random) -> bytes:
    if rng.random() < 0.45:
        while True:
            hw = rng.randrange(0xE000)
            if 0xD000 <= hw:                              # conditional branch / SVC
                continue
            if (hw & 0xFF00) == 0xBF00 and hw & 0xF:      # IT (needs its block)
                continue
            if (hw & 0xFF00) == 0x4700:                   # BX / BLX
                continue
            return struct.pack("<H", hw)
    while True:
        hw1 = rng.randrange(0xE800, 0x10000)
        if (hw1 >> 11) not in (0b11101, 0b11110, 0b11111):
            continue
        hw2 = rng.getrandbits(16)
        if (hw1 >> 11) == 0b11110 and hw2 & 0x8000:      # branches and misc control
            continue
        return struct.pack("<HH", hw1, hw2)


def thumb_stream(data: bytes):
    """Thumb instructions from a code stream (16- or 32-bit by the first halfword)."""
    out, i = [], 0
    while i + 2 <= len(data):
        hw = struct.unpack_from("<H", data, i)[0]
        size = 4 if (hw >> 11) in (0b11101, 0b11110, 0b11111) else 2
        if i + size <= len(data):
            out.append(data[i:i + size])
        i += size
    return out


def corpus_insns(paths):
    """ARM-v7 instructions from the executable sections of ELF libraries (read as Thumb)."""
    insns = []
    for p in paths:
        data = Path(p).read_bytes()
        if data[:4] != b"\x7fELF" or data[4] != 1:
            continue
        shoff, = struct.unpack_from("<I", data, 0x20)
        shentsize, shnum = struct.unpack_from("<HH", data, 0x2E)
        for k in range(shnum):
            sh = shoff + k * shentsize
            _, stype, flags, _, off, size = struct.unpack_from("<IIIIII", data, sh)
            if stype == 1 and flags & 0x4 and size:
                insns += thumb_stream(data[off:off + min(size, 4 << 20)])
    return insns


def random_state(rng: random.Random) -> dict:
    regs = []
    for _ in range(13):
        r = rng.random()
        if r < 0.5:
            regs.append(WINDOW + 0x100 + (rng.randrange(0x600) & ~3))
        elif r < 0.7:
            regs.append(rng.randrange(64))
        elif r < 0.8:
            regs.append(rng.choice([0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x8000, 0xFFFF]))
        else:
            regs.append(rng.getrandbits(32))
    d = []
    for _ in range(32):
        if rng.random() < 0.3:      # ordinary floats/doubles, so VFP arithmetic sees normal values
            f = struct.unpack("<I", struct.pack("<f", rng.uniform(-1000, 1000)))[0]
            d.append(f | (struct.unpack("<I", struct.pack("<f", rng.uniform(-10, 10)))[0] << 32)
                     if rng.random() < 0.5 else struct.unpack("<Q", struct.pack("<d", rng.uniform(-1e6, 1e6)))[0])
        else:
            d.append(rng.getrandbits(64))
    fpscr = rng.choice([0, 0, 0, 0x01000000, 0x02000000, 0x03000000, 0x00400000])   # FZ, DN, RMode
    return {"r": regs, "nzcv": rng.getrandbits(4), "q": rng.getrandbits(1), "ge": rng.getrandbits(4),
            "fpscr": fpscr, "d": d}


def make_cases(n: int, seed: int, corpus):
    rng = random.Random(seed)
    cases = []
    for _ in range(n):
        thumb = rng.random() < 0.5
        count = 1 if rng.random() < 0.8 else rng.choice([2, 3])
        if thumb:
            parts = [rng.choice(corpus) if corpus and rng.random() < 0.6 else thumb_insn(rng) for _ in range(count)]
            code = b"".join(parts)
        else:
            code = b"".join(struct.pack("<I", arm_word(rng)) for _ in range(count))
        cases.append({"thumb": thumb, "code": code, "state": random_state(rng)})
    return cases


# ------------------------------------------------------------------- execution ---

def case_line(case) -> str:
    s = case["state"]
    fields = [("1" if case["thumb"] else "0"), case["code"].hex()]
    fields += [f"{v:x}" for v in s["r"]]
    fields += [f"{s['nzcv']:x}", f"{s['q']:x}", f"{s['ge']:x}", f"{s['fpscr']:x}"]
    fields += [f"{v:x}" for v in s["d"]]
    return " ".join(fields)


def run_hvm(binary: str, cases):
    proc = subprocess.run([binary], input="\n".join(case_line(c) for c in cases) + "\n",
                          capture_output=True, text=True, timeout=600)
    lines = proc.stdout.splitlines()
    header = lines[0].split() if lines else []
    if len(header) != 2 or header[0] != "halt":
        raise SystemExit(f"[cpudiff] unexpected output from {binary}: {proc.stdout[:200]} {proc.stderr[:400]}")
    halt = int(header[1], 16)
    results = []
    for line in lines[1:]:
        f = line.split()
        if len(f) < 52:
            results.append({"status": f[0] if f else "other:empty"})
            continue
        results.append({"status": f[0], "r": [int(x, 16) for x in f[1:15]],
                        "nzcv": int(f[15], 16), "q": int(f[16], 16), "ge": int(f[17], 16), "fpscr": int(f[18], 16),
                        "d": [int(x, 16) for x in f[19:51]], "window": int(f[51], 16)})
    if len(results) != len(cases):
        raise SystemExit(f"[cpudiff] hvm_cpudiff answered {len(results)} of {len(cases)} cases\n{proc.stderr[-2000:]}")
    return halt, results


def run_unicorn(cases, halt: int):
    import unicorn
    from unicorn import arm_const as A
    from unicorn.unicorn import UcError
    pat = pattern()
    out = []
    for idx, case in enumerate(cases):
        at = CODE + idx * SLOT
        terminator = b"\x70\x47" if case["thumb"] else b"\x1e\xff\x2f\xe1"
        code = case["code"] + terminator
        uc = unicorn.Uc(unicorn.UC_ARCH_ARM, unicorn.UC_MODE_THUMB if case["thumb"] else unicorn.UC_MODE_ARM)
        page = at & ~0xFFF
        uc.mem_map(page, 0x2000, unicorn.UC_PROT_READ | unicorn.UC_PROT_EXEC)
        uc.mem_write(at, code)
        uc.mem_map(WINDOW, WINDOW_SIZE, unicorn.UC_PROT_READ | unicorn.UC_PROT_WRITE)
        uc.mem_write(WINDOW, pat)
        # VFP/NEON: on by default in Unicorn 2; older versions need CPACR and FPEXC set.
        for reg, value in ((getattr(A, "UC_ARM_REG_C1_C0_2", None), 0xF << 20), (getattr(A, "UC_ARM_REG_FPEXC", None), 0x40000000)):
            if reg is None:
                continue
            try:
                uc.reg_write(reg, uc.reg_read(reg) | value)
            except UcError:
                pass
        s = case["state"]
        for i, v in enumerate(s["r"]):
            uc.reg_write(A.UC_ARM_REG_R0 + i, v)
        uc.reg_write(A.UC_ARM_REG_SP, SP)
        uc.reg_write(A.UC_ARM_REG_LR, halt)
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR) & ~0xF80F0000
        uc.reg_write(A.UC_ARM_REG_CPSR, cpsr | (s["nzcv"] << 28) | (s["q"] << 27) | (s["ge"] << 16))
        uc.reg_write(A.UC_ARM_REG_FPSCR, s["fpscr"])
        for i, v in enumerate(s["d"]):
            uc.reg_write(A.UC_ARM_REG_D0 + i, v)
        end = at + len(case["code"])
        try:
            uc.emu_start(at | (1 if case["thumb"] else 0), end, count=16)
        except UcError as e:
            out.append({"status": "skip", "why": str(e)})
            continue
        if uc.reg_read(A.UC_ARM_REG_PC) != end:
            out.append({"status": "skip", "why": "left the sequence"})
            continue
        if uc.reg_read(A.UC_ARM_REG_LR) != halt:        # the sequence ends with bx lr
            out.append({"status": "skip", "why": "wrote lr"})
            continue
        cpsr = uc.reg_read(A.UC_ARM_REG_CPSR)
        out.append({"status": "ok", "r": [uc.reg_read(A.UC_ARM_REG_R0 + i) for i in range(13)] + [uc.reg_read(A.UC_ARM_REG_SP)],
                    "nzcv": cpsr >> 28, "q": (cpsr >> 27) & 1, "ge": (cpsr >> 16) & 0xF,
                    "fpscr": uc.reg_read(A.UC_ARM_REG_FPSCR),
                    "d": [uc.reg_read(A.UC_ARM_REG_D0 + i) for i in range(32)],
                    "window": fnv(bytes(uc.mem_read(WINDOW, WINDOW_SIZE)))})
    return out


def differences(h: dict, u: dict):
    diffs = []
    for i in range(14):
        if h["r"][i] != u["r"][i]:
            diffs.append(f"{'sp' if i == 13 else 'r%d' % i}: hvm {h['r'][i]:#x} unicorn {u['r'][i]:#x}")
    for k in ("nzcv", "q", "ge"):
        if h[k] != u[k]:
            diffs.append(f"{k}: hvm {h[k]:#x} unicorn {u[k]:#x}")
    if (h["fpscr"] & ~FPSCR_FLAGS) != (u["fpscr"] & ~FPSCR_FLAGS):
        diffs.append(f"fpscr: hvm {h['fpscr']:#x} unicorn {u['fpscr']:#x}")
    for i in range(32):
        if h["d"][i] != u["d"][i]:
            diffs.append(f"d{i}: hvm {h['d'][i]:#018x} unicorn {u['d'][i]:#018x}")
    if h["window"] != u["window"]:
        diffs.append("memory window differs")
    return diffs


def disassemble(case) -> str:
    tool = shutil.which("llvm-mc") or shutil.which("llvm-mc-18")
    if not tool:
        return ""
    triple = "thumbv7-none-eabi" if case["thumb"] else "armv7-none-eabi"
    text = " ".join(f"0x{b:02x}" for b in case["code"])
    try:
        out = subprocess.run([tool, "--disassemble", f"-triple={triple}", "-mattr=+neon,+vfp3,+fp16"],
                             input=text, capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.TimeoutExpired):
        return ""
    return "; ".join(l.strip() for l in out.splitlines() if l.strip() and not l.strip().startswith("."))


def mnemonic(asm: str) -> str:
    return asm.split(";")[0].split()[0] if asm.strip() else "?"


# ------------------------------------------------------------------------ main ---

def main() -> int:
    ap = argparse.ArgumentParser(description="HydrogenVM CPU against Unicorn")
    ap.add_argument("--hvm-cpudiff", default=str(ROOT / "build" / "hvm_cpudiff"))
    ap.add_argument("--cases", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=int(time.time()))
    ap.add_argument("--corpus", nargs="*", default=[], help="ELF libraries to draw real instructions from")
    ap.add_argument("--known", default=str(Path(__file__).with_name("known.json")),
                    help="mnemonics with accepted differences; only others fail the run")
    ap.add_argument("--out", default=str(ROOT / "build" / "cpudiff-report.json"))
    ap.add_argument("--batch", type=int, default=5000)
    args = ap.parse_args()

    try:
        import unicorn  # noqa: F401
    except ImportError:
        print("[cpudiff] needs the unicorn Python package (pip install unicorn)", file=sys.stderr)
        return 2
    if not Path(args.hvm_cpudiff).exists():
        print(f"[cpudiff] {args.hvm_cpudiff} not found; build the hvm_cpudiff target", file=sys.stderr)
        return 2

    corpus = corpus_insns(args.corpus)
    cases = make_cases(args.cases, args.seed, corpus)
    mismatches, unimplemented, skipped, compared = [], Counter(), Counter(), 0
    for start in range(0, len(cases), args.batch):
        batch = cases[start:start + args.batch]
        halt, hvm = run_hvm(args.hvm_cpudiff, batch)
        uni = run_unicorn(batch, halt)
        for case, h, u in zip(batch, hvm, uni):
            if u["status"] != "ok":
                skipped[u["why"].split(" (")[0]] += 1
                continue
            if h["status"] == "undef":
                unimplemented[disassemble(case) or case["code"].hex()] += 1
                continue
            compared += 1
            diffs = [f"hvm stopped: {h['status']}"] if h["status"] != "ok" else differences(h, u)
            if diffs:
                asm = disassemble(case)
                mismatches.append({"thumb": case["thumb"], "code": case["code"].hex(), "asm": asm,
                                   "mnemonic": mnemonic(asm), "state": case["state"], "diffs": diffs})

    known = set(json.loads(Path(args.known).read_text()).get("mnemonics", [])) if Path(args.known).exists() else set()
    by_mnemonic = defaultdict(list)
    for m in mismatches:
        by_mnemonic[m["mnemonic"]].append(m)
    new = {k: v for k, v in by_mnemonic.items() if k not in known}

    report = {"seed": args.seed, "cases": len(cases), "compared": compared, "skipped": dict(skipped),
              "unimplemented": dict(unimplemented.most_common(200)),
              "mismatchesByMnemonic": {k: len(v) for k, v in sorted(by_mnemonic.items(), key=lambda kv: -len(kv[1]))},
              "examples": {k: v[:3] for k, v in by_mnemonic.items()}, "new": sorted(new)}
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(report, indent=2))

    print(f"seed {args.seed}: {len(cases)} cases, {compared} compared, {sum(skipped.values())} skipped "
          f"(not valid or left the sequence), {sum(unimplemented.values())} unimplemented in HydrogenVM")
    for k, v in list(by_mnemonic.items())[:40]:
        ex = v[0]
        flag = "" if k in known else "  NEW"
        print(f"  {len(v):5d}  {k:<12} e.g. {ex['asm'] or ex['code']}: {ex['diffs'][0]}{flag}")
    if unimplemented:
        print("unimplemented (most common):")
        for asm, n in unimplemented.most_common(15):
            print(f"  {n:5d}  {asm}")
    print(f"report: {args.out}")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())
