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


def clean_git_env():
    # A caller's repository selection (for example from a git hook) must not redirect the fixture repositories.
    return {k: v for k, v in os.environ.items() if k not in ("GIT_DIR", "GIT_WORK_TREE", "GIT_INDEX_FILE", "GIT_COMMON_DIR")}


def git(repo, *args):
    return subprocess.check_output(["git", *args], cwd=repo, text=True, stderr=subprocess.DEVNULL, env=clean_git_env()).strip()


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
            git(repo, "-c", "commit.gpgsign=false", "-c", "core.hooksPath=/dev/null", "commit", "-qm", f"version {i}")
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

    def test_timed_out_unit_python_and_fuzz_cases_abort_instead_of_counting_as_bad(self):
        # tools/compat_harness/runner.py records these as a failed case whose summary is "timeout".
        for suite in ("unit", "python", "fuzz"):
            with self.subTest(suite=suite):
                self.root = Path(self.temp.name) / suite   # one subdirectory per subtest; setUp's cleanup removes the tree
                self.root.mkdir()
                repo, commits = self.repository()
                mark = (f"r['suites']['{suite}']['cases']=[{{'id':'t','status':'fail','exitCode':None,'durationSec':1,'summary':'timeout'}}]"
                        if suite == "fuzz" else
                        f"r['suites']['{suite}']['cases'][0].update(status='fail',summary='timeout')")
                self.runner.write_text(self.runner.read_text().replace("pathlib.Path(sys.argv[1])", mark + "\npathlib.Path(sys.argv[1])", 1))
                with contextlib.redirect_stderr(io.StringIO()) as errors:
                    self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
                self.assertIn("timed out", errors.getvalue())

    def test_runner_timeout_saves_logs_and_partial_output(self):
        repo, commits = self.repository()
        hang = self.root / "hang.py"
        hang.write_text("import sys,time\nprint('partial from hung runner',flush=True)\ntime.sleep(30)\n")
        args = self.bisect_args(repo, commits, [sys.executable, str(hang)])
        cut = args.index("--command")   # --command takes the rest of the line, so --timeout must come before it
        args = args[:cut] + ["--timeout", "1"] + args[cut:]
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(args), 2)
        logs = list((self.root / "out").glob("*/run.log"))
        self.assertTrue(logs, errors.getvalue())
        self.assertIn("partial from hung runner", logs[0].read_text())

    def test_changed_app_input_hash_aborts_instead_of_counting_as_bad(self):
        repo, commits = self.repository()
        self.runner.write_text(self.runner.read_text().replace(
            "pathlib.Path(sys.argv[1])", "r['suites']['apps']['apps'][0]['sha256']='e'*64\npathlib.Path(sys.argv[1])", 1))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(bisect_corpus.main(self.bisect_args(repo, commits)), 2)
        self.assertIn("sha256", errors.getvalue())

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

    def fake_hvm(self, name, value=0, write_report=True, exit_status=0):
        path = self.root / name
        path.write_text(f'''#!{sys.executable}
import json, pathlib, sys
args=sys.argv
def option(name): return pathlib.Path(args[args.index(name)+1])
home=option('--home')
assert not (home/'used').exists()
(home/'used').write_text('used')
print('guest result {value}')
report={{'exitStatus':{exit_status},'crashed':False,'loadError':'','perf':{{'wallSeconds':1,'framesPresented':1}},'audio':{{'frames':{value}}},'value':{value}}}
if {write_report!r}: option('--report').write_text(json.dumps(report))
option('--screenshot').write_bytes(bytes([{value}]))
option('--frame-file').write_bytes(bytes([{value}]))
sys.exit({exit_status})
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

    def test_invalid_ignore_log_regex_is_a_tool_error(self):
        hvm = self.fake_hvm("hvm")
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm) + ["--ignore-log-regex", "["]), 2)
        self.assertIn("invalid --ignore-log-regex", errors.getvalue())
        self.assertNotIn("Traceback", errors.getvalue())

    def test_git_helper_ignores_inherited_repository_variables(self):
        repo, _ = self.repository()
        other = self.root / "other"
        other.mkdir()
        subprocess.check_call(["git", "init", "-q", str(other)], stderr=subprocess.DEVNULL, env=clean_git_env())
        wanted = git(repo, "rev-parse", "--show-toplevel")
        env = {"GIT_DIR": str(other / ".git"), "GIT_WORK_TREE": str(other), "GIT_INDEX_FILE": str(other / ".git" / "index")}
        with mock.patch.dict(os.environ, env):
            self.assertEqual(git(repo, "rev-parse", "--show-toplevel"), wanted)

    def test_report_with_non_finite_numbers_is_rejected(self):
        hvm = self.fake_hvm("hvm")
        hvm.write_text(hvm.read_text().replace("json.dumps(report)", "json.dumps(report).replace('\"value\": 0', '\"value\": NaN')"))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 2)
        self.assertIn("no valid hvm report", errors.getvalue())

    def test_inherited_commit_signing_and_hooks_do_not_break_the_fixture_repository(self):
        hooks = self.root / "failing-hooks"
        hooks.mkdir()
        (hooks / "pre-commit").write_text("#!/bin/sh\nexit 1\n")
        (hooks / "pre-commit").chmod(0o755)
        env = {"GIT_CONFIG_COUNT": "3",
               "GIT_CONFIG_KEY_0": "commit.gpgSign", "GIT_CONFIG_VALUE_0": "true",
               "GIT_CONFIG_KEY_1": "gpg.program", "GIT_CONFIG_VALUE_1": "/bin/false",
               "GIT_CONFIG_KEY_2": "core.hooksPath", "GIT_CONFIG_VALUE_2": str(hooks)}
        with mock.patch.dict(os.environ, env):
            repo, commits = self.repository()
        self.assertEqual(len(commits), 10)

    def test_bisect_run_ignores_inherited_repository_variables(self):
        repo, _ = self.repository()
        other = self.root / "other-repo"
        other.mkdir()
        subprocess.check_call(["git", "init", "-q", str(other)], stderr=subprocess.DEVNULL, env=clean_git_env())
        env = {"GIT_DIR": str(other / ".git"), "GIT_WORK_TREE": str(other), "GIT_INDEX_FILE": str(other / ".git" / "index"),
               "GIT_COMMON_DIR": str(other / ".git")}
        with mock.patch.dict(os.environ, env):
            result = bisect_corpus.run(["git", "rev-parse", "--show-toplevel"], repo)
        self.assertEqual(result.returncode, 0, result.stdout)
        self.assertEqual(Path(result.stdout.strip()).resolve(), Path(repo).resolve())

    def test_fixture_init_works_when_the_caller_selects_another_repository(self):
        decoy = self.root / "decoy"
        decoy.mkdir()
        subprocess.check_call(["git", "init", "-q", str(decoy)], stderr=subprocess.DEVNULL, env=clean_git_env())
        with mock.patch.dict(os.environ, {"GIT_DIR": str(decoy / ".git"), "GIT_WORK_TREE": str(decoy)}):
            repo, commits = self.repository()
        self.assertEqual(len(commits), 10)
        self.assertEqual(Path(git(repo, "rev-parse", "--show-toplevel")).resolve(), Path(repo).resolve())
        self.assertNotEqual(Path(repo).resolve(), decoy.resolve())

    def test_crash_bundle_timing_and_emulator_commit_are_not_differences(self):
        def variant(name, wall, commit):
            hvm = self.fake_hvm(name)
            bundle = f"{{'perf': {{'wallSeconds': {wall}, 'frames': 3}}, 'host': {{'emulatorCommit': '{commit}', 'os': 'linux'}}}}"
            hvm.write_text(hvm.read_text().replace("json.dumps(report)", f"json.dumps(dict(report, crashBundle={bundle}))"))
            return hvm
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(variant("old", 1.5, "aaaa"), variant("new", 9.5, "bbbb"))), 0)
        raw = json.loads((self.root / "ab" / "new" / "report.json").read_text())
        self.assertEqual(raw["crashBundle"]["host"]["emulatorCommit"], "bbbb")   # kept in the raw report

    def test_report_with_infinite_floats_is_rejected(self):
        hvm = self.fake_hvm("hvm")
        hvm.write_text(hvm.read_text().replace("json.dumps(report)", "json.dumps(report).replace('\"wallSeconds\": 1', '\"wallSeconds\": 1e999')"))
        with contextlib.redirect_stderr(io.StringIO()) as errors:
            self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 2)
        self.assertIn("no valid hvm report", errors.getvalue())
        self.assertFalse((self.root / "ab" / "diff.json").exists())

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

    def test_real_crash_and_load_failure_exit_codes_are_measured_differences(self):
        for code, edit in ((70, ("'crashed':False", "'crashed':True")), (71, ("'loadError':''", "'loadError':'bad image'"))):
            with self.subTest(exit=code):
                self.root = Path(self.temp.name) / f"exit{code}"
                self.root.mkdir()
                old, new = self.fake_hvm("old"), self.fake_hvm("new", exit_status=code)
                new.write_text(new.read_text().replace(*edit))
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(ab_diff.main(self.ab_args(old, new)), 1)
                result = json.loads((self.root / "ab/diff.json").read_text())
                self.assertEqual([o["returncode"] for o in result["observations"]], [0, code])

    def test_exit_code_that_contradicts_its_report_is_still_a_tool_error(self):
        # 70 means "crashed" and 71 "load error"; a report saying neither is not a measurement.
        for code in (70, 71, 2):
            with self.subTest(exit=code):
                self.root = Path(self.temp.name) / f"bad{code}"
                self.root.mkdir()
                hvm = self.fake_hvm("hvm", exit_status=code)
                with contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(ab_diff.main(self.ab_args(hvm, hvm)), 2)

    def test_valid_load_failure_is_a_measured_difference(self):
        old, new = self.fake_hvm("old"), self.fake_hvm("new", exit_status=1)
        new.write_text(new.read_text().replace("'loadError':''", "'loadError':'unimplemented import'"))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(ab_diff.main(self.ab_args(old, new)), 1)
        result = json.loads((self.root / "ab/diff.json").read_text())
        self.assertEqual([o["returncode"] for o in result["observations"]], [0, 1])


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
