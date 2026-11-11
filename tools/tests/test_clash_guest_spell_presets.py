import tempfile
import unittest
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from unittest.mock import patch

from clash_sandbox_compare import compare
from clash_spell_preset import DEFAULTS, FIELDS, FORMAT, decode, encode, write_new


class SpellPresetTests(unittest.TestCase):
    definitions = [{"id": 28000000, "caps": 27, "name": "Fireball"},
                   {"id": 28000001, "caps": 0, "name": "Mirror fixture"}]

    def preset(self):
        rows = [dict(id=entry["id"], **dict(zip(FIELDS, DEFAULTS))) for entry in self.definitions]
        rows[0]["radius"] = 175
        return dict(format=FORMAT, name="Wide", selected=0, enabled=True, spells=rows)

    def test_profile_and_short_prefix_round_trip(self):
        preset = self.preset()
        for count in (2, 1):
            preset["spells"] = preset["spells"][:count]
            self.assertEqual(decode(encode(preset, self.definitions), self.definitions, "Wide"), preset)

    def test_rejects_incompatible_and_damaged_inputs(self):
        for key, value in (("id", 28000002), ("cost", 1), ("damage", False)):
            preset = self.preset()
            preset["spells"][1][key] = value
            with self.assertRaises(ValueError):
                encode(preset, self.definitions)
        binary = encode(self.preset(), self.definitions)
        for invalid in (binary[:-1], binary + b"x", binary[:24] + bytes(4) + binary[28:]):
            with self.assertRaises(ValueError):
                decode(invalid, self.definitions, "Wide")

    def test_compare_uses_independent_homes(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            (root / "app").mkdir()
            (root / "app/Info.plist").touch()
            home = root / "original"
            documents = home / "Documents"
            documents.mkdir(parents=True)
            (documents / "game").write_bytes(b"earned")
            plan = root / "plan.json"
            plan.write_text('{"steps": []}')
            def simulate(args, check):
                copy = Path(args[args.index("--home") + 1]) / "Documents/game"
                self.assertEqual(copy.read_bytes(), b"earned")
                copy.write_bytes(b"changed")
            with patch("clash_sandbox_compare.subprocess.run", side_effect=simulate) as runner:
                compare("hvm", root / "app", home, plan, root / "results", None, 60)
                self.assertEqual(runner.call_count, 2)
                with self.assertRaises(ValueError):
                    compare("hvm", root / "app", home, plan, home / "results", None, 60)
                with self.assertRaises(FileExistsError):
                    compare("hvm", root / "app", home, plan, root / "results", None, 60)
            self.assertEqual((documents / "game").read_bytes(), b"earned")

    def test_existing_output_is_preserved(self):
        with tempfile.TemporaryDirectory() as root:
            path = Path(root) / "preset.json"
            write_new(path, b"original")
            with self.assertRaises(FileExistsError):
                write_new(path, b"replace")
            self.assertEqual(path.read_bytes(), b"original")
            self.assertEqual(list(path.parent.iterdir()), [path])
