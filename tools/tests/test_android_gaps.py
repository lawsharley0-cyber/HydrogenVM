"""tools/android_gaps.py: DEX and binary-manifest parsing, engine detection, the ledger and the
placeholder tables the runtime compiles in."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))

import android_gaps as ag  # noqa: E402


def uleb(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        out.append(b | (0x80 if n else 0))
        if not n:
            return bytes(out)


def build_dex() -> bytes:
    """A minimal classes.dex: class Lcom/example/Game; with a native `nativeInit(I)V` and a
    plain `run()V`, plus a reference to android.app.Activity.finish()."""
    strings = ["I", "Lcom/example/Game;", "V", "VI", "nativeInit", "run", "Landroid/app/Activity;", "finish"]
    types = [0, 1, 2, 6]                         # I, Game, V, Activity (string indices)
    header = 0x70
    string_ids_off = header
    type_ids_off = string_ids_off + 4 * len(strings)
    proto_ids_off = type_ids_off + 4 * len(types)
    protos = 2
    method_ids_off = proto_ids_off + 12 * protos
    methods = 3
    class_defs_off = method_ids_off + 8 * methods
    data_off = class_defs_off + 32
    data = bytearray()
    string_offs = []
    for s in strings:
        string_offs.append(data_off + len(data))
        data += uleb(len(s)) + s.encode() + b"\0"
    while (data_off + len(data)) % 4:
        data += b"\0"
    typelist_off = data_off + len(data)
    data += struct.pack("<IH", 1, 0) + b"\0\0"                 # (I)
    class_data_off = data_off + len(data)
    # 0 static, 0 instance fields; 2 direct methods (sorted by method index), 0 virtual.
    data += uleb(0) + uleb(0) + uleb(2) + uleb(0)
    data += uleb(0) + uleb(0x100 | 0x1) + uleb(0)             # method 0: nativeInit, ACC_NATIVE
    data += uleb(1) + uleb(0x1) + uleb(0)                     # method 1: run
    body = bytearray()
    body += b"".join(struct.pack("<I", o) for o in string_offs)
    body += b"".join(struct.pack("<I", t) for t in types)
    body += struct.pack("<III", 3, 2, typelist_off)              # proto 0: (I)V, shorty "VI"
    body += struct.pack("<III", 2, 2, 0)                         # proto 1: ()V, shorty "V"
    body += struct.pack("<HHI", 1, 0, 4)                         # Game.nativeInit (I)V
    body += struct.pack("<HHI", 1, 1, 5)                         # Game.run ()V
    body += struct.pack("<HHI", 3, 1, 7)                         # Activity.finish ()V
    body += struct.pack("<8I", 1, 1, 0xFFFFFFFF, 0, 0xFFFFFFFF, 0, class_data_off, 0)
    head = bytearray(b"dex\n035\0" + b"\0" * (header - 8))
    struct.pack_into("<12I", head, 0x38, len(strings), string_ids_off, len(types), type_ids_off, protos,
                     proto_ids_off, 0, 0, methods, method_ids_off, 1, class_defs_off)
    return bytes(head + body + data)


def build_axml() -> bytes:
    """<manifest package="com.example.game"><activity name="android.app.NativeActivity"/></manifest>"""
    strings = ["manifest", "package", "com.example.game", "activity", "name", "android.app.NativeActivity"]
    pool = bytearray()
    offs = []
    for s in strings:
        offs.append(len(pool))
        pool += struct.pack("<H", len(s)) + s.encode("utf-16-le") + b"\0\0"
    while len(pool) % 4:
        pool += b"\0"
    sp_header = 28
    sp_size = sp_header + 4 * len(strings) + len(pool)
    sp = struct.pack("<HHI", 0x0001, sp_header, sp_size) + struct.pack("<IIIII", len(strings), 0, 0, sp_header + 4 * len(strings), 0)
    sp += b"".join(struct.pack("<I", o) for o in offs) + pool

    def element(name: int, attr_name: int, value: int) -> bytes:
        attr = struct.pack("<IIIHBBI", 0xFFFFFFFF, attr_name, value, 8, 0, 0x03, value)
        ext = struct.pack("<IIHHHHHH", 0xFFFFFFFF, name, 20, 20, 1, 0, 0, 0)
        node = struct.pack("<HHI", 0x0102, 16, 16 + len(ext) + len(attr)) + struct.pack("<II", 1, 0xFFFFFFFF)
        return node + ext + attr

    body = element(0, 1, 2) + element(3, 4, 5)
    total = 8 + len(sp) + len(body)
    return struct.pack("<HHI", 0x0003, 8, total) + sp + body


class DexTests(unittest.TestCase):
    def test_native_methods_and_framework_refs(self):
        info = ag.parse_dex(build_dex())
        self.assertEqual(info.classes, 1)
        self.assertEqual(info.methods, 3)
        self.assertEqual(info.natives, [("com/example/Game", "nativeInit", "(I)V")])
        self.assertEqual(info.framework_refs, {"android/app/Activity": 1})

    def test_rejects_other_files(self):
        with self.assertRaises(ValueError):
            ag.parse_dex(b"PK\x03\x04" + b"\0" * 200)


class ManifestTests(unittest.TestCase):
    def test_package_and_activity(self):
        elements = ag.parse_axml(build_axml())
        self.assertEqual(elements[0], ("manifest", {"package": "com.example.game"}))
        self.assertEqual(elements[1], ("activity", {"name": "android.app.NativeActivity"}))


class EngineTests(unittest.TestCase):
    def test_engines(self):
        self.assertEqual(ag.detect_engine({"libunity.so", "libmono.so", "libmain.so"}, "", set(), False), ("Unity (Mono)", "driver"))
        self.assertEqual(ag.detect_engine({"libil2cpp.so", "libunity.so"}, "", set(), False), ("Unity (IL2CPP)", "none"))
        self.assertEqual(ag.detect_engine({"libgame.so"}, "Lcom/ea/blast/MainActivity;", set(), False), ("EA Blast", "driver"))
        self.assertEqual(ag.detect_engine({"libcocos2dcpp.so"}, "", set(), False)[0], "Cocos2d-x")
        self.assertEqual(ag.detect_engine({"libgame.so"}, "", {"ANativeActivity_onCreate"}, False), ("NativeActivity", "planned"))
        self.assertEqual(ag.detect_engine(set(), "", set(), False), ("Java only", "none"))

    def test_native_kind(self):
        self.assertEqual(ag.native_kind("socket"), "fail")
        self.assertEqual(ag.native_kind("epoll_wait"), "fail")
        self.assertEqual(ag.native_kind("glTexGenfOES"), "zero")


def game(name, native=(), jni=()):
    return {"apk": name + ".apk", "package": name, "engine": "Unity (Mono)", "support": "driver", "notes": [],
            "nativeMissing": [{"symbol": s, "library": "libx.so", "weak": False} for s in native],
            "runtimeMissing": {"jni": [{"class": c, "name": n, "sig": s, "count": 1} for c, n, s in jni],
                               "native": [], "other": []}}


class LedgerTests(unittest.TestCase):
    def test_ranked_by_games(self):
        games = [game("a", ["socket", "glFoo"], [("com/x/Ads", "show", "()V")]),
                 game("b", ["socket"], [("com/x/Ads", "show", "()V"), ("com/y/Iap", "init", "(Ljava/lang/String;)V")])]
        ledger = ag.build_ledger(games)
        self.assertEqual(ledger["native"][0]["key"], "socket")
        self.assertEqual(ledger["native"][0]["games"], ["a", "b"])
        self.assertEqual(ledger["native"][0]["kind"], "fail")
        self.assertEqual(ledger["jni"][0]["key"], "com/x/Ads.show()V")
        self.assertIn("socket", ag.ledger_markdown(ledger, games))

    def test_placeholders_merge_and_round_trip(self):
        ledger = ag.build_ledger([game("a", ["socket", "glFoo"], [("com/x/Ads", "show", "()V")])])
        jni, native = ag.placeholders_from_ledger(ledger, {("com/z/Old", "keep", "()Z")}, {"oldFn": "Zero"})
        self.assertIn(("com/z/Old", "keep", "()Z"), jni)          # never removes an entry
        self.assertIn(("com/x/Ads", "show", "()V"), jni)
        self.assertEqual(native, {"oldFn": "Zero", "socket": "Fail", "glFoo": "Zero"})
        with tempfile.TemporaryDirectory() as tmp:
            j, n = Path(tmp) / "jni.inc", Path(tmp) / "native.inc"
            j.write_text(ag.format_jni_inc(jni))
            n.write_text(ag.format_native_inc(native))
            self.assertEqual(ag.read_jni_inc(j), jni)
            self.assertEqual(ag.read_native_inc(n), native)

    def test_committed_tables_are_well_formed(self):
        """Every entry line of the tables the runtime compiles in parses (hand edits included)."""
        for name, pattern in ((ag.JNI_INC, ag._JNI_LINE), (ag.NATIVE_INC, ag._NATIVE_LINE)):
            path = ag.PLACEHOLDER_DIR / name
            self.assertTrue(path.exists(), path)
            for line in path.read_text().splitlines():
                if line.strip() and not line.lstrip().startswith("//"):
                    self.assertRegex(line, pattern, f"{name}: {line}")


if __name__ == "__main__":
    unittest.main()
