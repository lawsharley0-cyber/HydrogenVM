"""tools/cpudiff: case generation and the HydrogenVM side of the differential CPU check
(the Unicorn side runs in .github/workflows/cpudiff.yml)."""
import struct
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "cpudiff"))

import cpudiff  # noqa: E402

HVM = ROOT / "build" / "hvm_cpudiff"


class GenerationTests(unittest.TestCase):
    def test_deterministic(self):
        a, b = cpudiff.make_cases(200, 7, []), cpudiff.make_cases(200, 7, [])
        self.assertEqual([c["code"] for c in a], [c["code"] for c in b])

    def test_thumb_stream_lengths(self):
        code = struct.pack("<HHHH", 0x4018, 0xF04F, 0x0001, 0xBF00)   # ands; mov.w r0,#1; nop
        self.assertEqual(cpudiff.thumb_stream(code), [code[0:2], code[2:6], code[6:8]])

    def test_case_line_shape(self):
        case = cpudiff.make_cases(1, 3, [])[0]
        self.assertEqual(len(cpudiff.case_line(case).split()), 2 + 13 + 4 + 32)


@unittest.skipUnless(HVM.exists(), "build the hvm_cpudiff target")
class HvmSideTests(unittest.TestCase):
    def case(self, thumb, code, regs, nzcv=0):
        state = {"r": regs + [0] * (13 - len(regs)), "nzcv": nzcv, "q": 0, "ge": 0, "fpscr": 0, "d": [0] * 32}
        return {"thumb": thumb, "code": bytes.fromhex(code), "state": state}

    def test_adds_and_flags(self):
        _, (arm, thumb) = cpudiff.run_hvm(str(HVM), [self.case(False, "010090e0", [0xFFFFFFFF, 1]),
                                                     self.case(True, "4018", [5, 7])])
        self.assertEqual((arm["status"], arm["r"][0], arm["nzcv"]), ("ok", 0, 0x6))   # Z and C
        self.assertEqual((thumb["status"], thumb["r"][0]), ("ok", 12))   # 0x1840: adds r0, r0, r1

    def test_jump_to_top_of_memory_is_a_guest_fault(self):
        # mvn pc, pc, lsr r8 with a shift >= 32: PC = 0xFFFFFFFF, a Thumb jump to 0xFFFFFFFE.
        _, (r,) = cpudiff.run_hvm(str(HVM), [self.case(False, "3ff8e5e1", [0] * 8 + [0x60])])
        self.assertEqual(r["status"], "fault:fffffffe")


if __name__ == "__main__":
    unittest.main()
