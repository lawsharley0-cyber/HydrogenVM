#!/usr/bin/env python3
"""Cross-check HydrogenVM's Mach-O loader against llvm-objdump.

For each binary: every import llvm-objdump reports (dyld-info bind/lazy-bind
tables, or indirect symbol pointers for classic binaries) must appear in the
loader's bind list at the same address, and the entry point must match.
usage: tools/check_macho.py <hvm_machodump> <binary>...
"""
import re, subprocess, sys

def objdump(*args):
    return subprocess.run(["llvm-objdump", "--macho", *args], capture_output=True, text=True).stdout

def expected_binds(path):
    exp = {}
    out = objdump("--bind", "--lazy-bind", path)
    for l in out.splitlines():
        m = re.match(r"^\S+\s+\S+\s+(0x[0-9A-Fa-f]+)\s+(?:\S+\s+\S+\s+)?\S+\s+(\S+)\s*$", l)
        if m:
            exp[int(m.group(1), 16)] = m.group(2)
    if exp:
        return exp
    # Classic binaries: symbol pointer sections via the indirect symbol table.
    out = objdump("--indirect-symbols", path)
    sect = None
    for l in out.splitlines():
        m = re.match(r"^Indirect symbols for \((\S+),(\S+)\)", l)
        if m:
            sect = m.group(2)
            continue
        m = re.match(r"^(0x[0-9a-fA-F]+)\s+(\d+|LOCAL|ABSOLUTE)\s*(\S*)", l)
        if m and sect in ("__nl_symbol_ptr", "__la_symbol_ptr") and m.group(3):
            exp[int(m.group(1), 16)] = m.group(3)
    return exp

def main():
    tool, files = sys.argv[1], sys.argv[2:]
    bad = 0
    for f in files:
        out = subprocess.run([tool, f], capture_output=True, text=True).stdout
        if out.startswith("error"):
            print(f"FAIL {f}: {out.strip()}"); bad += 1; continue
        got = {}
        for l in out.splitlines():
            m = re.match(r"^bind (0x[0-9a-f]+) (\S+)", l)
            if m:
                got[int(m.group(1), 16)] = m.group(2)
        exp = expected_binds(f)
        missing = {a: s for a, s in exp.items() if got.get(a) != s}
        hdr = objdump("--private-headers", f)
        entry_ok = True
        m = re.search(r"entryoff (\d+)", hdr)
        line0 = out.splitlines()[0]
        if m:
            text = re.search(r"segname __TEXT\s+vmaddr (0x[0-9a-f]+)", hdr)
            want = int(text.group(1), 16) + int(m.group(1))
            entry_ok = int(re.search(r"entry (0x[0-9a-f]+)", line0).group(1), 16) == want
        else:
            pc = re.search(r"\bpc (0x[0-9a-f]+)", hdr)
            if pc:
                entry_ok = int(re.search(r"entry (0x[0-9a-f]+)", line0).group(1), 16) & ~1 == int(pc.group(1), 16) & ~1
        status = "ok" if not missing and entry_ok else "FAIL"
        if status != "ok":
            bad += 1
        print(f"{status:4s} {f}: {len(exp)} imports checked, {len(got)} binds found, entry {'ok' if entry_ok else 'MISMATCH'}")
        for a, s in list(missing.items())[:10]:
            print(f"     missing/mismatched 0x{a:08x} {s} (got {got.get(a)})")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
