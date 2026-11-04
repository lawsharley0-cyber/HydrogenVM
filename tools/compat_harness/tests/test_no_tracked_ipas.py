import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..")))

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))


class NoTrackedIpaTests(unittest.TestCase):
    def test_no_ipa_is_tracked_in_git(self):
        try:
            out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            self.skipTest("not a git checkout")
        tracked = [p for p in out.decode("utf-8", "replace").split("\0") if p.lower().endswith(".ipa")]
        self.assertEqual(tracked, [], "real-game IPAs belong in the private store (#204), not in git")

    def test_workflows_do_not_read_root_ipas(self):
        wf = os.path.join(ROOT, ".github", "workflows")
        for name in sorted(os.listdir(wf)):
            if name == "clash-royale.yml":
                continue  # reads a pinned historical commit, not the working tree
            with open(os.path.join(wf, name), encoding="utf-8") as fh:
                text = fh.read()
            for ipa in ("Tiny_Tower_v1.0_os30.ipa", "AngryBirds_v1.0_os30.ipa", "Art_Of_Glow_v1.1.2_os313.ipa"):
                if name == "private-fixture-availability.yml":
                    self.assertNotIn(f"unzip -q {ipa}", text)
                    continue
                self.assertNotIn(ipa, text, f"{name} still reads {ipa} from the repository")

    def test_harness_workflow_does_not_allow_legacy_repo_ipas(self):
        with open(os.path.join(ROOT, ".github", "workflows", "compat-harness.yml"), encoding="utf-8") as fh:
            text = fh.read()
        self.assertNotIn("--allow-legacy-repo", text)
        self.assertNotIn('HVM_ALLOW_LEGACY_IPAS: "1"', text)


if __name__ == "__main__":
    unittest.main()
