import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest

SPEC = importlib.util.spec_from_file_location("lint_stubs", Path(__file__).resolve().parents[1] / "lint_stubs.py")
lint = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = lint
SPEC.loader.exec_module(lint)


class StubLintTests(unittest.TestCase):
    def test_known_cases_are_labelled(self):
        root = Path(os.environ.get("HVM_STUB_SOURCE_ROOT", str(lint.ROOT)))
        cases = {
            "src/runtime/libc/System.cpp": {"_mprotect", "_madvise", "_msync"},
            "src/runtime/uikit/OpenGLES.cpp": {"_glDrawBuffers", "_glReadBuffer", "_glGetQueryObjectuivEXT", "_glGetQueryObjectuiv"},
            "src/runtime/uikit/PlatformStubs.cpp": {"_AUGraphGetIndNode", "_AUGraphGetNodeCount", "_AudioUnitAddRenderNotify", "isPlayable"},
        }
        violations = []
        for name, known in cases.items():
            violations.extend(f.registration for f in lint.scan((root / name).read_text(encoding="utf-8"), name)
                              if f.registration in known)
        self.assertEqual(violations, [], "T3's known constant-success registrations require explicit metadata")

    def test_multiline_body_and_every_alias(self):
        source = '''int ok(Ctx&, int, int) {
            // A } in a comment is not the end of the body.
            return 0;
        }
        p.def<&ok>("_first");
        supportDef<&ok>(p, "_labelled", SupportStatus::Stub, "No effect");
        p.def<&ok>("_second");'''
        self.assertEqual([f.registration for f in lint.scan(source)], ["_first", "_second"])

    def test_objc_capability_and_method_function(self):
        source = '''int yes(Ctx&, id, SEL) { return YES; }
        c.m<&yes>("isReady", "c@:");
        c.method("isPlayable", [](Ctx& x) { x.cpu.r[0] = 1; }, "c@:");
        c.classMethod("canRun", [](Ctx& x) { x.cpu.r[0] = true; });'''
        self.assertEqual([f.registration for f in lint.scan(source)], ["isReady", "isPlayable", "canRun"])

    def test_strings_comments_raw_literals_and_nested_scopes(self):
        source = '''// p.def<&ok>("_comment");
        const char* s = R"tag(p.def<&ok>("_raw"); { })tag";
        const char* t = "return 0; }";
        int ok(Ctx&) { if (true) { /* } */ } return 0; }
        p.def<&ok>("_actual");'''
        self.assertEqual([f.registration for f in lint.scan(source)], ["_actual"])

    def test_computed_result_is_not_constant_success(self):
        self.assertEqual(lint.scan('int f(Ctx& x) { return x.read(); } p.def<&f>("_f");'), [])

    def test_noop_and_fabricated_output(self):
        source = '''void noop(Ctx&) {} p.def<&noop>("_noop");
        void query(Ctx& x, int p) { if (p) x.mem.write<uint32_t>(p, 1); }
        p.def<&query>("_query");'''
        self.assertEqual(len(lint.scan(source)), 2)

    def test_host_function_named_and_lambda(self):
        source = '''int ok(Ctx&) { return 0; }
        p.defineFunction("_named", ok);
        p.defineFunction("_lambda", [](Ctx& x) { x.cpu.r[0] = 0; });'''
        self.assertEqual(len(lint.scan(source)), 2)

    def test_explicit_label_is_per_registration(self):
        source = '''int yes(Ctx&) { return 1; }
        supportDef<&yes>(p, "_yes", SupportStatus::Real, "Independent oracle");
        supportMethod(p, c, "Class", "ready", [](Ctx& x) { x.cpu.r[0] = 1; },
                      SupportStatus::Stub, "Unconditional readiness");'''
        self.assertEqual(lint.scan(source), [])

    def test_baseline_reports_debt_and_rejects_new_or_changed_body(self):
        with tempfile.TemporaryDirectory() as tmp, contextlib.redirect_stdout(io.StringIO()) as output:
            root = Path(tmp)
            runtime = root / "src/runtime"
            runtime.mkdir(parents=True)
            source = runtime / "stub.cpp"
            baseline = root / "baseline.json"
            source.write_text('int f(Ctx&) { return 0; } p.def<&f>("_f");')
            self.assertEqual(lint.main(["--root", tmp]), 1)
            self.assertEqual(lint.main(["--root", tmp, "--write-baseline", str(baseline)]), 0)
            self.assertEqual(lint.main(["--root", tmp, "--baseline", str(baseline)]), 0)
            self.assertIn("UNLABELLED _f", output.getvalue())
            self.assertEqual(len(json.loads(baseline.read_text())["findings"]), 1)
            source.write_text('\n\nint f(Ctx&) { return 0; } p.def<&f>("_f");')
            self.assertEqual(lint.main(["--root", tmp, "--baseline", str(baseline)]), 0)
            source.write_text('int f(Ctx&) { return 0; } p.def<&f>("_f"); p.def<&f>("_f");')
            self.assertEqual(lint.main(["--root", tmp, "--baseline", str(baseline)]), 1)
            source.write_text('int f(Ctx&) { return 1; } p.def<&f>("_f");')
            self.assertEqual(lint.main(["--root", tmp, "--baseline", str(baseline)]), 1)


if __name__ == "__main__":
    unittest.main()
