#!/usr/bin/env python3
"""HydrogenVM Phase I4: Ahead-of-Time Recompiled Packs generator.

Translates recorded hot ARMv7 basic blocks into portable C++20 functions with
register-promoted locals so Clang/LLVM compiles them ahead of time into native
ARM64 code signed into HydrogenVM (100% JIT-less). Any instruction outside the
AOT subset, indirect jump, or binary SHA-256 hash mismatch falls back cleanly to
the interpreter.

Usage:
  python3 tools/aot_pack.py generate --profile blocks.json --out deadspace_pack.cpp
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path
from typing import Any


def fnv1a64(data: bytes) -> int:
    h = 14695981039346656037
    for b in data:
        h ^= b
        h = (h * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h or 1


def _ror32(val: int, rot: int) -> int:
    rot &= 31
    if rot == 0:
        return val & 0xFFFFFFFF
    return ((val >> rot) | (val << (32 - rot))) & 0xFFFFFFFF


def _arm_imm(imm12: int) -> int:
    rot = ((imm12 >> 8) & 0xF) * 2
    return _ror32(imm12 & 0xFF, rot)


def _cond_expr(cond: int) -> str | None:
    # NZCV in bits 31..28: N=31, Z=30, C=29, V=28
    if cond == 0xE:
        return None  # unconditional
    n = "((nzcv >> 31) & 1u)"
    z = "((nzcv >> 30) & 1u)"
    c = "((nzcv >> 29) & 1u)"
    v = "((nzcv >> 28) & 1u)"
    table = {
        0x0: f"({z} != 0u)",
        0x1: f"({z} == 0u)",
        0x2: f"({c} != 0u)",
        0x3: f"({c} == 0u)",
        0x4: f"({n} != 0u)",
        0x5: f"({n} == 0u)",
        0x6: f"({v} != 0u)",
        0x7: f"({v} == 0u)",
        0x8: f"({c} != 0u && {z} == 0u)",
        0x9: f"({c} == 0u || {z} != 0u)",
        0xA: f"({n} == {v})",
        0xB: f"({n} != {v})",
        0xC: f"({z} == 0u && {n} == {v})",
        0xD: f"({z} != 0u || {n} != {v})",
    }
    return table.get(cond)


def _decode_arm_insn(pc: int, word: int) -> tuple[list[str], bool]:
    """Translate one ARMv7 instruction to C++ statements.

    Returns (lines, terminates_block). Returns ([], False) if unsupported
    (caller emits an interpreter fallback exit at `pc`).
    """
    cond = (word >> 28) & 0xF
    if cond == 0xF:
        return [], False
    cond_str = _cond_expr(cond)

    def wrap(body: list[str]) -> list[str]:
        if cond_str is None:
            return body
        return [f"if ({cond_str}) {{"] + [f"    {line}" for line in body] + ["}"]

    # BX Rm (0x012FFF10 mask 0x0FFFFFF0)
    if (word & 0x0FFFFFF0) == 0x012FFF10:
        rm = word & 0xF
        body = [f"next_key = r[{rm}];", "goto exit_block;"]
        return wrap(body), cond_str is None

    # B / BL imm24
    if (word & 0x0E000000) == 0x0A000000:
        link = bool(word & 0x01000000)
        imm24 = word & 0x00FFFFFF
        if imm24 & 0x00800000:
            imm24 -= 0x01000000
        target = (pc + 8 + (imm24 << 2)) & 0xFFFFFFFF
        body = []
        if link:
            body.append(f"r[14] = 0x{(pc + 4) & 0xFFFFFFFF:08X}u;")
        body.append(f"next_key = 0x{target:08X}u;")
        body.append("goto exit_block;")
        return wrap(body), cond_str is None

    # Data processing immediate (bits 27:25 == 001) or register (000, bit4==0, shift==0)
    op_class = (word >> 25) & 0x7
    if op_class in (0b001, 0b000):
        if op_class == 0b000 and ((word & 0x00000FF0) != 0):
            return [], False  # shifted/misc reg ops fall back to interpreter
        opc = (word >> 21) & 0xF
        s_bit = bool((word >> 20) & 1)
        rn = (word >> 16) & 0xF
        rd = (word >> 12) & 0xF
        if rd == 15 or rn == 15:
            return [], False
        op2 = f"0x{_arm_imm(word & 0xFFF):08X}u" if op_class == 0b001 else f"r[{word & 0xF}]"

        if opc == 0xD:  # MOV
            body = [f"r[{rd}] = {op2};"]
            if s_bit:
                body.append(f"nzcv = (nzcv & 0x30000000u) | (r[{rd}] & 0x80000000u) | (r[{rd}] == 0u ? 0x40000000u : 0u);")
            return wrap(body), False
        if opc == 0xF:  # MVN
            body = [f"r[{rd}] = ~({op2});"]
            if s_bit:
                body.append(f"nzcv = (nzcv & 0x30000000u) | (r[{rd}] & 0x80000000u) | (r[{rd}] == 0u ? 0x40000000u : 0u);")
            return wrap(body), False
        if opc == 0x4:  # ADD
            body = [
                f"{{ uint32_t a = r[{rn}], b = {op2}; uint64_t sum = uint64_t(a) + uint64_t(b); uint32_t res = uint32_t(sum); r[{rd}] = res;"
            ]
            if s_bit:
                body.append(
                    "  uint32_t ov = (~(a ^ b) & (a ^ res)) >> 31;"
                    "  nzcv = (res & 0x80000000u) | (res == 0u ? 0x40000000u : 0u) | ((sum >> 32) ? 0x20000000u : 0u) | (ov << 28);"
                )
            body.append("}")
            return wrap(body), False
        if opc in (0x2, 0xA):  # SUB / CMP
            body = [
                f"{{ uint32_t a = r[{rn}], b = {op2}; uint32_t res = a - b;"
            ]
            if opc == 0x2:
                body.append(f"  r[{rd}] = res;")
            if s_bit or opc == 0xA:
                body.append(
                    "  uint32_t ov = ((a ^ b) & (a ^ res)) >> 31;"
                    "  nzcv = (res & 0x80000000u) | (res == 0u ? 0x40000000u : 0u) | (a >= b ? 0x20000000u : 0u) | (ov << 28);"
                )
            body.append("}")
            return wrap(body), False
        if opc in (0x0, 0xC, 0x1):  # AND / ORR / EOR
            sym = {0x0: "&", 0xC: "|", 0x1: "^"}[opc]
            body = [f"r[{rd}] = r[{rn}] {sym} {op2};"]
            if s_bit:
                body.append(f"nzcv = (nzcv & 0x30000000u) | (r[{rd}] & 0x80000000u) | (r[{rd}] == 0u ? 0x40000000u : 0u);")
            return wrap(body), False

    # Single word LDR / STR immediate offset (P=1, B=0, W=0)
    if (word & 0x0F700000) == 0x05100000 or (word & 0x0F700000) == 0x05000000:
        is_load = bool((word >> 20) & 1)
        is_add = bool((word >> 23) & 1)
        rn = (word >> 16) & 0xF
        rd = (word >> 12) & 0xF
        imm12 = word & 0xFFF
        if rd == 15 or rn == 15:
            return [], False
        addr_expr = f"(r[{rn}] {'+' if is_add else '-'} 0x{imm12:X}u)"
        if is_load:
            body = [f"{{ uint32_t v; std::memcpy(&v, mem + {addr_expr}, 4); r[{rd}] = v; }}"]
        else:
            body = [f"{{ uint32_t v = r[{rd}]; std::memcpy(mem + {addr_expr}, &v, 4); }}"]
        return wrap(body), False

    return [], False


def generate_aot_pack_cpp(profile: dict[str, Any]) -> str:
    """Emit a self-contained C++20 AOT Recompiled Pack source file."""
    game = str(profile.get("game", "unknown"))
    binary_sha256 = str(profile.get("binary_sha256", "")).lower()
    if len(binary_sha256) != 64:
        raise ValueError("profile.binary_sha256 must be a 64-char hex SHA-256")
    pack_id = str(profile.get("pack_id", "hvm_aot_pack"))
    blocks = profile.get("blocks", [])
    if not isinstance(blocks, list) or not blocks:
        raise ValueError("profile.blocks must be a non-empty list")

    lines = [
        f"// Auto-generated HydrogenVM Phase I4 AOT Recompiled Pack for {game}",
        "// Compiled ahead of time into native machine code; falls back to interpreter on mismatch.",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <cstring>",
        "",
        f"namespace {pack_id} {{",
        "",
        "struct AotCpuState {",
        "    uint32_t r[16];",
        "    uint32_t nzcv;",
        "    uint32_t thumb;",
        "};",
        "",
        "using AotBlockFn = uint32_t (*)(AotCpuState* cpu, uint8_t* mem);",
        "",
        "struct AotBlockEntry {",
        "    uint32_t key;",
        "    uint32_t end;",
        "    uint64_t code_hash;",
        "    AotBlockFn fn;",
        "};",
        "",
        "struct AotPackDescriptor {",
        "    const char* game;",
        "    const char* binary_sha256;",
        "    const AotBlockEntry* blocks;",
        "    size_t block_count;",
        "};",
        "",
    ]

    entries: list[tuple[int, int, int, str]] = []
    for blk in blocks:
        pc = int(blk["pc"])
        words = [int(w) & 0xFFFFFFFF for w in blk["words"]]
        if not words:
            continue
        raw_bytes = b"".join(struct.pack("<I", w) for w in words)
        code_hash = fnv1a64(raw_bytes)
        end_pc = (pc + len(words) * 4) & 0xFFFFFFFF
        fn_name = f"aot_block_{pc:08x}"

        lines.append(f"static uint32_t {fn_name}(AotCpuState* cpu, uint8_t* mem) {{")
        lines.append("    uint32_t r[16];")
        lines.append("    std::memcpy(r, cpu->r, sizeof(r));")
        lines.append("    uint32_t nzcv = cpu->nzcv;")
        lines.append(f"    uint32_t next_key = 0x{end_pc:08X}u;")
        terminated = False
        cur_pc = pc
        for w in words:
            stmts, term = _decode_arm_insn(cur_pc, w)
            if not stmts:
                lines.append(f"    // Fallback to interpreter at unsupported insn 0x{cur_pc:08X}: 0x{w:08X}")
                lines.append(f"    next_key = 0x{cur_pc:08X}u;")
                lines.append("    goto exit_block;")
                terminated = True
                break
            for s in stmts:
                lines.append(f"    {s}")
            cur_pc = (cur_pc + 4) & 0xFFFFFFFF
            if term:
                terminated = True
                break
        if not terminated:
            lines.append("    goto exit_block;")
        lines.append("exit_block:")
        lines.append("    std::memcpy(cpu->r, r, sizeof(r));")
        lines.append("    cpu->nzcv = nzcv;")
        lines.append("    cpu->r[15] = next_key & ~1u;")
        lines.append("    cpu->thumb = next_key & 1u;")
        lines.append("    return next_key;")
        lines.append("}")
        lines.append("")
        entries.append((pc, end_pc, code_hash, fn_name))

    lines.append("static const AotBlockEntry kBlocks[] = {")
    for pc, end_pc, code_hash, fn_name in entries:
        lines.append(f"    {{ 0x{pc:08X}u, 0x{end_pc:08X}u, 0x{code_hash:016X}ULL, &{fn_name} }},")
    lines.append("};")
    lines.append("")
    lines.append("inline const AotPackDescriptor& descriptor() {")
    lines.append(f'    static const AotPackDescriptor kDesc = {{ "{game}", "{binary_sha256}", kBlocks, {len(entries)} }};')
    lines.append("    return kDesc;")
    lines.append("}")
    lines.append("")
    lines.append("inline AotBlockFn lookupBlock(const char* live_sha256, uint32_t key, uint64_t live_code_hash) {")
    lines.append("    const auto& d = descriptor();")
    lines.append("    if (!live_sha256 || std::strcmp(live_sha256, d.binary_sha256) != 0) return nullptr;")
    lines.append("    for (size_t i = 0; i < d.block_count; ++i) {")
    lines.append("        if (d.blocks[i].key == key && d.blocks[i].code_hash == live_code_hash) return d.blocks[i].fn;")
    lines.append("    }")
    lines.append("    return nullptr;")
    lines.append("}")
    lines.append("")
    lines.append(f"}} // namespace {pack_id}")
    lines.append("")
    return "\n".join(lines)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="HydrogenVM Phase I4 AOT Recompiled Pack generator")
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_gen = sub.add_parser("generate", help="Generate a C++20 AOT pack from a recorded block profile JSON")
    p_gen.add_argument("--profile", type=Path, required=True)
    p_gen.add_argument("--out", type=Path, required=True)

    args = parser.parse_args(argv)
    if args.cmd == "generate":
        profile = json.loads(args.profile.read_text(encoding="utf-8"))
        cpp = generate_aot_pack_cpp(profile)
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(cpp, encoding="utf-8")
        print(f"Generated AOT pack {args.out} ({len(profile.get('blocks', []))} blocks)")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
