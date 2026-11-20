"""Unit tests for Phase H1 (tools/fix_agent.py) and Phase H2 (tools/pack_sign.py)."""

from __future__ import annotations

import copy
import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import fix_agent  # noqa: E402
import pack_sign  # noqa: E402


class SelfImprovingLoopTests(unittest.TestCase):
    def test_fix_agent_ranks_regressions_and_guards_hotspots(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            ledger = Path(td) / "gaps.json"
            nightly = Path(td) / "status.json"
            ledger.write_text(
                json.dumps(
                    {
                        "gaps": [
                            {
                                "symbol": "java/io/Foo.bar",
                                "games": ["g1", "g2", "g3"],
                                "count": 50,
                                "files": ["src/runtime/android/JavaStdlib.cpp"],
                            },
                            {
                                "symbol": "covered_api",
                                "games": ["g1", "g2", "g3", "g4"],
                                "count": 100,
                                "files": ["src/runtime/android/JavaStdlib.cpp"],
                            },
                            {
                                "symbol": "hotspot_api",
                                "games": ["g1", "g2", "g3", "g4", "g5"],
                                "count": 200,
                                "files": ["src/runtime/Process.cpp"],
                            },
                        ]
                    }
                ),
                encoding="utf-8",
            )
            nightly.write_text(
                json.dumps(
                    {
                        "regressions": [
                            {
                                "game": "Alto",
                                "symptom": "black_screen",
                                "count": 1,
                                "files": ["src/runtime/uikit/OpenGLES.cpp"],
                            }
                        ]
                    }
                ),
                encoding="utf-8",
            )
            cands = fix_agent.load_candidates(ledger, nightly)
            picked = fix_agent.select_candidates(
                cands,
                open_pr_titles=["Implement covered_api for Android"],
                locked_files={"src/runtime/Process.cpp"},
                max_tasks=2,
            )
            self.assertEqual(len(picked), 2)
            # Regression always beats gaps
            self.assertEqual(picked[0].kind, "regression")
            self.assertIn("Alto", picked[0].title)
            # covered_api and locked hotspot_api are skipped -> java/io/Foo.bar wins
            self.assertEqual(picked[1].key, "gap:java/io/Foo.bar")

    def test_signed_improvement_pack_accepts_valid_and_refuses_tampered(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            priv = Path(td) / "priv.pem"
            pub = Path(td) / "pub.pem"
            other_priv = Path(td) / "other_priv.pem"
            other_pub = Path(td) / "other_pub.pem"
            pack_sign.generate_ed25519_keypair(priv, pub)
            pack_sign.generate_ed25519_keypair(other_priv, other_pub)

            payload = {
                "schema_version": 1,
                "pack_version": "2026.11.20-1",
                "sections": {
                    "placeholders": [{"symbol": "com/ads/Sdk.init", "return": 0}],
                    "compat_profiles": [{"game": "Example", "gles1_emulate": True}],
                },
            }
            signed = pack_sign.sign_pack(payload, priv)

            # 1. Valid signature verifies cleanly
            ok, msg = pack_sign.verify_pack(signed, pub)
            self.assertTrue(ok, msg)

            # 2. Tampered payload is strictly refused
            tampered = copy.deepcopy(signed)
            tampered["payload"]["sections"]["placeholders"][0]["return"] = 1
            ok_tamper, _ = pack_sign.verify_pack(tampered, pub)
            self.assertFalse(ok_tamper)

            # 3. Wrong public key is refused
            ok_wrong_key, _ = pack_sign.verify_pack(signed, other_pub)
            self.assertFalse(ok_wrong_key)

            # 4. Unsigned envelope is refused
            unsigned = copy.deepcopy(signed)
            del unsigned["signature"]
            ok_unsigned, _ = pack_sign.verify_pack(unsigned, pub)
            self.assertFalse(ok_unsigned)


if __name__ == "__main__":
    unittest.main()
