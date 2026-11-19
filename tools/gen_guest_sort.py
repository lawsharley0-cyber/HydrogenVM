#!/usr/bin/env python3
"""Generate src/runtime/libc/GuestSort.inc containing PIC ARMv7 implementations of
qsort, qsort_r (Darwin/BSD argument order), and bsearch.

Running these loops as guest ARMv7 code eliminates the host<->guest round trip
(Engine::callGuest) on every comparator invocation (plan I7 #6).
"""
import subprocess
import sys
import tempfile
from pathlib import Path

GUEST_SORT_C = r'''
#include <stddef.h>
#include <stdint.h>

static inline void swap_elem(uint8_t *a, uint8_t *b, size_t size) {
    if (a == b) return;
    if ((((uintptr_t)a | (uintptr_t)b | size) & 3u) == 0u) {
        uint32_t *wa = (uint32_t *)a;
        uint32_t *wb = (uint32_t *)b;
        size_t words = size >> 2;
        while (words--) {
            uint32_t t = *wa;
            *wa++ = *wb;
            *wb++ = t;
        }
        return;
    }
    while (size--) {
        uint8_t t = *a;
        *a++ = *b;
        *b++ = t;
    }
}

typedef int (*cmp_fn)(const void *, const void *);
typedef int (*cmp_r_fn)(void *, const void *, const void *);

static void sift_down(uint8_t *base, size_t start, size_t end, size_t size, cmp_fn cmp) {
    size_t root = start;
    for (;;) {
        size_t child = root * 2u + 1u;
        if (child >= end) break;
        size_t best = root;
        if (cmp(base + best * size, base + child * size) < 0) best = child;
        if (child + 1u < end && cmp(base + best * size, base + (child + 1u) * size) < 0)
            best = child + 1u;
        if (best == root) break;
        swap_elem(base + root * size, base + best * size, size);
        root = best;
    }
}

__attribute__((visibility("default"), section(".text.qsort")))
void guest_qsort(void *vbase, size_t n, size_t size, cmp_fn cmp) {
    if (!vbase || n < 2u || !size || !cmp) return;
    uint8_t *base = (uint8_t *)vbase;
    size_t start = n / 2u;
    while (start > 0u) {
        --start;
        sift_down(base, start, n, size, cmp);
    }
    size_t end = n;
    while (end > 1u) {
        --end;
        swap_elem(base, base + end * size, size);
        sift_down(base, 0u, end, size, cmp);
    }
}

static void sift_down_r(uint8_t *base, size_t start, size_t end, size_t size, void *thunk, cmp_r_fn cmp) {
    size_t root = start;
    for (;;) {
        size_t child = root * 2u + 1u;
        if (child >= end) break;
        size_t best = root;
        if (cmp(thunk, base + best * size, base + child * size) < 0) best = child;
        if (child + 1u < end && cmp(thunk, base + best * size, base + (child + 1u) * size) < 0)
            best = child + 1u;
        if (best == root) break;
        swap_elem(base + root * size, base + best * size, size);
        root = best;
    }
}

__attribute__((visibility("default"), section(".text.qsort_r")))
void guest_qsort_r(void *vbase, size_t n, size_t size, void *thunk, cmp_r_fn cmp) {
    if (!vbase || n < 2u || !size || !cmp) return;
    uint8_t *base = (uint8_t *)vbase;
    size_t start = n / 2u;
    while (start > 0u) {
        --start;
        sift_down_r(base, start, n, size, thunk, cmp);
    }
    size_t end = n;
    while (end > 1u) {
        --end;
        swap_elem(base, base + end * size, size);
        sift_down_r(base, 0u, end, size, thunk, cmp);
    }
}

__attribute__((visibility("default"), section(".text.bsearch")))
void *guest_bsearch(const void *key, const void *vbase, size_t n, size_t size, cmp_fn cmp) {
    if (!vbase || !n || !size || !cmp) return NULL;
    const uint8_t *base = (const uint8_t *)vbase;
    size_t lo = 0u, hi = n;
    while (lo < hi) {
        size_t mid = lo + ((hi - lo) >> 1);
        const uint8_t *elem = base + mid * size;
        int r = cmp(key, elem);
        if (r == 0) return (void *)elem;
        if (r < 0) hi = mid;
        else lo = mid + 1u;
    }
    return NULL;
}
'''


def main() -> int:
    repo = Path(__file__).resolve().parent.parent
    out_path = repo / "src/runtime/libc/GuestSort.inc"

    with tempfile.TemporaryDirectory() as td:
        td_path = Path(td)
        c_file = td_path / "guest_sort.c"
        o_file = td_path / "guest_sort.o"
        elf_file = td_path / "guest_sort.elf"
        bin_file = td_path / "guest_sort.bin"
        ld_file = td_path / "link.ld"

        c_file.write_text(GUEST_SORT_C)
        ld_file.write_text(
            "SECTIONS {\n"
            "  . = 0x0;\n"
            "  .text : { *(.text.qsort) *(.text.qsort_r) *(.text.bsearch) *(.text*) }\n"
            "  /DISCARD/ : { *(.ARM.*) *(.comment) *(.note*) }\n"
            "}\n"
        )

        subprocess.run(
            [
                "clang",
                "--target=armv7-none-eabi",
                "-marm",
                "-march=armv7-a",
                "-mfloat-abi=soft",
                "-O2",
                "-ffreestanding",
                "-fno-builtin",
                "-fno-stack-protector",
                "-fno-unwind-tables",
                "-fno-asynchronous-unwind-tables",
                "-c",
                str(c_file),
                "-o",
                str(o_file),
            ],
            check=True,
        )

        # Verify no undefined external symbols.
        nm_undef = subprocess.check_output(["llvm-nm", "-u", str(o_file)], text=True).strip()
        if nm_undef:
            sys.stderr.write(f"ERROR: undefined symbols in guest_sort.o:\n{nm_undef}\n")
            return 1

        subprocess.run(
            ["ld.lld", "-T", str(ld_file), "--nostdlib", str(o_file), "-o", str(elf_file)],
            check=True,
        )
        subprocess.run(
            ["llvm-objcopy", "-O", "binary", "-j", ".text", str(elf_file), str(bin_file)],
            check=True,
        )

        syms = {}
        nm_out = subprocess.check_output(["llvm-nm", str(elf_file)], text=True)
        for line in nm_out.splitlines():
            parts = line.split()
            if len(parts) == 3 and parts[2] in ("guest_qsort", "guest_qsort_r", "guest_bsearch"):
                syms[parts[2]] = int(parts[0], 16)

        for req in ("guest_qsort", "guest_qsort_r", "guest_bsearch"):
            if req not in syms:
                sys.stderr.write(f"ERROR: missing symbol {req}\n")
                return 1

        raw = bin_file.read_bytes()
        assert len(raw) % 4 == 0 and len(raw) <= 4096

        lines = [
            "// Generated by tools/gen_guest_sort.py — do not edit by hand.",
            "// Position-independent ARMv7 (A32) guest implementations of qsort, qsort_r,",
            "// and bsearch so comparator calls stay inside the interpreter loop (plan I7 #6).",
            "namespace {",
            f"constexpr uint32_t kGuestQsortOffset = 0x{syms['guest_qsort']:X}u;",
            f"constexpr uint32_t kGuestQsortROffset = 0x{syms['guest_qsort_r']:X}u;",
            f"constexpr uint32_t kGuestBsearchOffset = 0x{syms['guest_bsearch']:X}u;",
            f"constexpr uint8_t kGuestSortCode[{len(raw)}] = {{",
        ]
        for i in range(0, len(raw), 16):
            chunk = ", ".join(f"0x{b:02X}" for b in raw[i : i + 16])
            lines.append(f"    {chunk},")
        lines.append("};")
        lines.append("} // namespace")
        lines.append("")

        out_path.write_text("\n".join(lines))
        print(f"Wrote {out_path} ({len(raw)} bytes): {syms}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
