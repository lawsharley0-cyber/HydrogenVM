#!/usr/bin/env python3
"""Summarise an HVM_PROFILE_PAIRS run: most frequent consecutive handler pairs.
usage: tools/pairprof.py <hvm binary built with -DHVM_PROFILE_PAIRS> <pairprof.txt> [top]"""
import subprocess, sys, re
binary, prof = sys.argv[1], sys.argv[2]
top = int(sys.argv[3]) if len(sys.argv) > 3 else 40
syms = {}
for line in subprocess.run(["nm", "-C", binary], capture_output=True, text=True).stdout.splitlines():
    m = re.match(r"([0-9a-f]+) [tTwW] (.*)", line)
    if m: syms[int(m.group(1), 16)] = m.group(2)
base = next(a for a, n in syms.items() if n.startswith("Hydrogen::CPU::hvmEnterBlock("))
def name(off):
    a = base + off - 0x80000000
    n = syms.get(a, hex(a))
    n = n.replace("Hydrogen::CPU::", "").replace("(Hydrogen::CPU::Cpu*, Hydrogen::CPU::Op const*, unsigned char*)", "")
    return n.replace("void ", "")
rows, single = [], {}
for line in open(prof):
    a, b, n = line.split()
    a, b, n = int(a, 16), int(b, 16), int(n)
    rows.append((n, name(a), name(b)))
    single[name(a)] = single.get(name(a), 0) + n
total = sum(r[0] for r in rows)
print(f"{total/1e6:.1f}M transitions\n\nTop ops:")
for k, v in sorted(single.items(), key=lambda x: -x[1])[:25]:
    print(f"  {100*v/total:5.1f}%  {k}")
print("\nTop pairs:")
for n, a, b in sorted(rows, reverse=True)[:top]:
    print(f"  {100*n/total:5.1f}%  {a}  ->  {b}")
