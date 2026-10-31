import contextlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import ab_diff
import bisect_corpus
import hvm_report


def git(repo, *args):
    return subprocess.check_output(["git", *args], cwd=repo, text=True, stderr=subprocess.DEVNULL).strip()


@unittest.skipUnless(os.name == "posix", "hvm tooling integration requires Linux/macOS")
class CorpusToolsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def repository(self):
        repo = self.root / "repo"
        repo.mkdir()
        git(repo, "init", "-q")
        git(repo, "config", "user.name", "Corpus test")
        git(repo, "config", "user.email", "corpus@example.invalid")
        template = json.loads((TOOLS.parent / "docs/dev-system/baselines/linux-headless.json").read_text())
        self.baseline = self.root / "baseline.json"
        self.baseline.write_text(json.dumps(template))
        (repo / "template.json").write_text(json.dumps(template))
        self.runner = self.root / "gate.py"
        self.runner.write_text('''import json, pathlib, sys
r=json.loads(pathlib.Path('template.json').read_text())
bad=int(pathlib.Path('version').read_text()) >= 5
r['summary']['ok']=not bad
if bad:
 r['summary']['unitFailed']=1
 r['suites']['unit']['cases'][0]['status']='fail'
pathlib.Path(sys.argv[1]).write_text(json.dumps(r))
sys.exit(int(bad))
''')
        commits = []
        for i in range(10):
            (repo / "version").write_text(str(i))
            git(repo, "add", ".")
            git(repo, "commit", "-qm", f"version {i}")
            commits.append(git(repo, "rev-parse", "HEAD"))
        return repo, commits

    def bisect_args(self, repo, commits, command=None):
        return ["--repo", str(repo), "--good", commits[0], "--bad", commits[-1],
                "--baseline", str(self.baseline), "--out", str(self.root / "out"),
                "--command", *(command or [sys.executable, str(self.runner), "{report}"])]

    def test_ten_commits_find_planted_regression_without_touching_checkout(self):
        repo, commits = self.repository()
        (repo / "dirty.txt").write_text("preserve me")
        before = git(repo, "status", "--porcelain")
        with contextlib.redirect_stdout(io.StringIO()):
            rc = bisect_corpus.main(self.bisect_args(repo, commits))
        self.assertEqual(rc, 0)
        result = json.loads((self.root / "out/result.json").read_text())
        self.assertEqual(result["firstBadCommit"], commits[5])
        self.assertEqual(git(repo, "rev-parse", "HEAD"), commits[-1])
        self.assertEqual(git(repo, "status", "--porcelain"), before)
        self.assertEqual(len(git(repo, "worktree", "list", "--porcelain").split("worktree ")) - 1, 1)

    def test_missing_runner_is_an_error_not_a_bad_commit(self):
        repo, commits = self.repository()
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            rc = bisect_corpus.main(self.bisect_args(repo, commits, [str(self.root / "missing")]))
        self.assertEqual(rc, 2)
        self.assertIn("could not execute", errors.getvalue())
        self.assertFalse((self.root / "out/result.json").exists())

    def test_exit_zero_without_report_is_not_success(self):
        repo, commits = self.repository()
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits, [sys.executable, "-c", "pass"])), 2)

    def test_bad_endpoint_must_actually_fail(self):
        repo, commits = self.repository()
        self.runner.write_text("import pathlib,sys\npathlib.Path(sys.argv[1]).write_text(pathlib.Path('template.json').read_text())\n")
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("bad endpoint passes", errors.getvalue())

    def test_missing_corpus_is_infrastructure_failure(self):
        repo, commits = self.repository()
        self.runner.write_text(self.runner.read_text().replace("r['summary']['ok']=not bad", "r['summary']['ok']=not bad\nr['summary']['resolveErrors']=1"))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("fixtures could not be resolved", errors.getvalue())

    def test_timed_out_app_aborts_instead_of_counting_as_bad(self):
        repo, commits = self.repository()
        self.runner.write_text(self.runner.read_text().replace(
            "pathlib.Path(sys.argv[1])", "r['suites']['apps']['apps'][0]['status']='timeout'\npathlib.Path(sys.argv[1])", 1))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("timed out", errors.getvalue())

    def test_timeout_status_in_unit_and_python_cases_is_rejected_as_invalid(self):
        for suite in ("unit", "python"):
            with self.subTest(suite=suite):
                self.temp.cleanup(); self.temp = tempfile.TemporaryDirectory(); self.root = Path(self.temp.name)
                repo, commits = self.repository()
                self.runner.write_text(self.runner.read_text().replace(
                    "pathlib.Path(sys.argv[1])", f"r['suites']['{suite}']['cases'][0]['status']='timeout'\npathlib.Path(sys.argv[1])", 1))
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
                self.assertIn("invalid harness report", errors.getvalue())

    def test_duplicate_app_ids_with_different_hashes_abort(self):
        repo, commits = self.repository()
        self.runner.write_text(self.runner.read_text().replace(
            "pathlib.Path(sys.argv[1])",
            "a=r['suites']['apps']['apps']\nd=dict(a[0]); d['sha256']='f'*64; a.append(d)\npathlib.Path(sys.argv[1])", 1))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("duplicate", errors.getvalue())

    def test_omitted_cases_do_not_become_a_regression(self):
        repo, commits = self.repository()
        self.runner.write_text(self.runner.read_text().replace("pathlib.Path(sys.argv[1])", "r['suites']['unit']['cases']=[]\npathlib.Path(sys.argv[1])"))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("omitted baseline cases", errors.getvalue())

    def test_midpoint_infrastructure_failure_aborts_bisect(self):
        repo, commits = self.repository()
        self.runner.write_text("from pathlib import Path\nimport sys\nif Path('version').read_text()=='4': sys.exit(2)\n" + self.runner.read_text())
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("bisect failed/aborted", errors.getvalue())
        self.assertIn("infrastructure exit 2", (self.root / "out/bisect.log").read_text())

    def fake_hvm(self, name, value=0, write_report=True):
        path = self.root / name
        path.write_text(f'''#!{sys.executable}
import json, pathlib, sys
args=sys.argv
def option(name): return pathlib.Path(args[args.index(name)+1])
home=option('--home')
assert not (home/'used').exists()
(home/'used').write_text('used')
print('guest result {value}')
report={{'exitStatus':0,'crashed':False,'loadError':'','perf':{{'wallSeconds':1,'framesPresented':1}},'audio':{{'frames':{value}}},'value':{value}}}
if {write_report!r}: option('--report').write_text(json.dumps(report))
option('--screenshot').write_bytes(bytes([{value}]))
option('--frame-file').write_bytes(bytes([{value}]))
''')
        path.chmod(0o755)
        return path

    def ab_args(self, old, new):
        guest = self.root / "guest.app"
        guest.mkdir(exist_ok=True)
        return [str(old), str(new), "--guest", str(guest), "--out", str(self.root / "ab"),
                "--tap", "10,20@1", "--drag", "1,2,3,4@2", "--lifecycle", "bg@3,fg@4"]

    def test_identical_builds_have_isolated_homes_and_identical_scripts(self):
        hvm = self.fake_hvm("hvm")
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 0)
        out = self.root / "ab"
        result = json.loads((out / "diff.json").read_text())
        self.assertTrue(result["summary"]["ok"])
        commands = [json.loads((out / side / "command.json").read_text()) for side in ("old", "new")]
        self.assertEqual(commands[0][commands[0].index("--tap"):], commands[1][commands[1].index("--tap"):])
        self.assertNotEqual(commands[0][commands[0].index("--home")+1], commands[1][commands[1].index("--home")+1])

    def test_planted_change_reports_every_observation_category(self):
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(self.fake_hvm("old"), self.fake_hvm("new", 1))), 1)
        result = json.loads((self.root / "ab/diff.json").read_text())
        for key in ("report", "logs", "frameHashes", "audioCounters"):
            self.assertIsNotNone(result["firstDivergences"][key])

    def test_ab_missing_report_is_error(self):
        hvm = self.fake_hvm("hvm", write_report=False)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 2)

    def test_ab_missing_report_names_exit_status_and_log(self):
        hvm = self.fake_hvm("hvm", write_report=False)
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 2)
        self.assertIn("no valid hvm report (exit 0)", errors.getvalue())
        self.assertIn("stderr.log", errors.getvalue())

    def test_ab_timeout_keeps_partial_output_and_reports_it(self):
        hang = self.root / "hang"
        hang.write_text(f"#!{sys.executable}\nimport sys,time\nprint('partial out',flush=True)\nprint('partial err',file=sys.stderr,flush=True)\ntime.sleep(30)\n")
        hang.chmod(0o755)
        args = self.ab_args(hang, hang) + ["--timeout", "2", "--seconds", "1"]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(ab_diff.main(args), 2)
        self.assertIn("timed out after 2", errors.getvalue())
        self.assertEqual((self.root / "ab/old/stdout.log").read_text().strip(), "partial out")
        self.assertEqual((self.root / "ab/old/stderr.log").read_text().strip(), "partial err")

    def test_valid_load_failure_is_a_measured_difference(self):
        old, new = self.fake_hvm("old"), self.fake_hvm("new")
        new.write_text(new.read_text().replace("'loadError':''", "'loadError':'unimplemented import'"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(old, new)), 1)


class ComparisonTests(unittest.TestCase):
    def test_existing_report_runner_records_tool_failures(self):
        with mock.patch.object(hvm_report, "suites", {}), mock.patch.object(hvm_report, "problems", []), \
             mock.patch.object(hvm_report, "run", side_effect=[(1, "", "tool test failed", 0), (1, "", "guest build failed", 0)]) as run:
            hvm_report.do_cpu("unused-build")
            self.assertIn("test_corpus_tools.py", run.call_args_list[0].args[0])
            self.assertEqual(hvm_report.suites["tools"]["failed"], 1)
            self.assertEqual(hvm_report.problems[0]["message"], "tool test failed")

    def test_first_path_and_missing_keys(self):
        self.assertEqual(ab_diff.first_difference({"a":[1,2]}, {"a":[1,3]})["path"], "$.a[1]")
        self.assertEqual(ab_diff.first_difference({}, {"a":None})["missing"], "old")
        self.assertIsNone(ab_diff.first_difference([1], [1]))

    def test_only_known_sandbox_identity_is_normalized(self):
        text = '/var/mobile/Applications/12345678-1234-1234-1234-123456789abc/Documents/result'
        self.assertIn('<home-id>', ab_diff.normalize(text, []))
        self.assertEqual(ab_diff.normalize('guest value 123456', []), 'guest value 123456')


if __name__ == "__main__":
    unittest.main()
