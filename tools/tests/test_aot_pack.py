"""Unit tests for tools/aot_pack.py (Phase I4 AOT Recompiled Packs)."""

from __future__ import annotations

import hashlib
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import aot_pack  # noqa: E402


class AotPackTests(unittest.TestCase):
    def test_generate_compile_and_execute_aot_pack(self) -> None:
        # ARMv7 function at 0x1000: sum 1..r0 into r1, store r1 to [r2], return r1 in r0.
        # Block 1 (0x1000): mov r1, #0; b 0x1008
        # Block 2 (0x1008): cmp r0, #0; beq 0x1018; add r1, r1, r0; sub r0, r0, #1; b 0x1008
        # Block 3 (0x1018): str r1, [r2, #0]; mov r0, r1; bx lr
        blk1_words = [0xE3A01000, 0xEAFFFFFE + 1]  # mov r1, #0; b 0x1008 (pc=0x1004, +8=0x100C, imm=-1 -> 0x1008)
        blk2_words = [
            0xE3500000,  # cmp r0, #0
            0x0A000002,  # beq 0x1018 (pc=0x100C, +8=0x1014, +8=0x101C? wait: pc=0x100C -> +8=0x1014 -> +4=0x1018 => imm=1)
            0xE0811000,  # add r1, r1, r0
            0xE2400001,  # sub r0, r0, #1
            0xEAFFFFFA,  # b 0x1008 (pc=0x1018? wait: blk2 has 5 insns -> 0x1008..0x101C)
        ]
        # Fix exact branch targets:
        # 0x1000: mov r1, #0        (0xE3A01000)
        # 0x1004: b 0x1008          (pc+8=0x100C, target=0x1008 => imm=-1 => 0xEAFFFFFF)
        # 0x1008: cmp r0, #0        (0xE3500000)
        # 0x100C: beq 0x101C        (pc+8=0x1014, target=0x101C => imm=+2 => 0x0A000002)
        # 0x1010: add r1, r1, r0    (0xE0811000)
        # 0x1014: sub r0, r0, #1    (0xE2400001)
        # 0x1018: b 0x1008          (pc+8=0x1020, target=0x1008 => imm=-6 => 0xEAFFFFFA)
        # 0x101C: str r1, [r2, #0]  (0xE5821000)
        # 0x1020: mov r0, r1        (0xE1A00001)
        # 0x1024: bx lr             (0xE12FFF1E)
        blk1_words = [0xE3A01000, 0xEAFFFFFF]
        blk2_words = [0xE3500000, 0x0A000002, 0xE0811000, 0xE2400001, 0xEAFFFFFA]
        blk3_words = [0xE5821000, 0xE1A00001, 0xE12FFF1E]

        sha = hashlib.sha256(b"dead-space-fixture-binary").hexdigest()
        profile = {
            "game": "Dead Space (test)",
            "pack_id": "test_pack",
            "binary_sha256": sha,
            "blocks": [
                {"pc": 0x1000, "words": blk1_words},
                {"pc": 0x1008, "words": blk2_words},
                {"pc": 0x101C, "words": blk3_words},
            ],
        }
        cpp = aot_pack.generate_aot_pack_cpp(profile)
        self.assertIn(sha, cpp)
        self.assertIn("aot_block_00001000", cpp)

        compiler = shutil.which("clang++") or shutil.which("g++")
        if not compiler:
            self.skipTest("no host C++ compiler available")

        with tempfile.TemporaryDirectory() as td:
            pack_hpp = Path(td) / "pack.hpp"
            driver_cpp = Path(td) / "driver.cpp"
            bin_path = Path(td) / "driver"
            pack_hpp.write_text(cpp, encoding="utf-8")

            h1 = aot_pack.fnv1a64(b"".join(struct.pack("<I", w) for w in blk1_words))
            h2 = aot_pack.fnv1a64(b"".join(struct.pack("<I", w) for w in blk2_words))
            h3 = aot_pack.fnv1a64(b"".join(struct.pack("<I", w) for w in blk3_words))

            driver_cpp.write_text(
                f"""
#include "pack.hpp"
#include <cassert>
#include <vector>

int main() {{
    const char* good_sha = "{sha}";
    const char* bad_sha  = "0000000000000000000000000000000000000000000000000000000000000000";
    // Mismatched binary hash or code hash must be rejected (fallback to interpreter)
    assert(test_pack::lookupBlock(bad_sha, 0x1000u, 0x{h1:016X}ULL) == nullptr);
    assert(test_pack::lookupBlock(good_sha, 0x1000u, 0x1234ULL) == nullptr);

    std::vector<uint8_t> mem(4096, 0);
    test_pack::AotCpuState cpu{{}};
    cpu.r[0] = 10;     // sum 1..10 = 55
    cpu.r[2] = 0x100;  // output memory address
    cpu.r[14] = 0xDEADBEEFu;

    uint32_t pc = 0x1000u;
    for (int steps = 0; steps < 64 && pc != 0xDEADBEEFu; ++steps) {{
        uint64_t h = (pc == 0x1000u) ? 0x{h1:016X}ULL : (pc == 0x1008u ? 0x{h2:016X}ULL : 0x{h3:016X}ULL);
        auto fn = test_pack::lookupBlock(good_sha, pc, h);
        assert(fn != nullptr);
        pc = fn(&cpu, mem.data());
    }}
    assert(pc == 0xDEADBEEFu);
    assert(cpu.r[0] == 55u);
    uint32_t stored = 0;
    std::memcpy(&stored, mem.data() + 0x100, 4);
    assert(stored == 55u);
    return 0;
}}
""",
                encoding="utf-8",
            )
            subprocess.run([compiler, "-std=c++20", "-O2", str(driver_cpp), "-o", str(bin_path)], check=True)
            subprocess.run([str(bin_path)], check=True)


if __name__ == "__main__":
    unittest.main()
