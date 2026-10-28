#!/usr/bin/env python3
import sys
import os
import unittest
from pathlib import Path
import tempfile
import json
from unittest.mock import patch, MagicMock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

import hvm_memory_soak

class TestMemorySoak(unittest.TestCase):
    def test_telemetry_adapter(self):
        adapter = hvm_memory_soak.TelemetryAdapter(os.getpid(), interval=0.1)
        if hvm_memory_soak.psutil:
            adapter.start()
            import time
            time.sleep(0.3)
            adapter.stop()
            samples = adapter.get_samples()
            self.assertTrue(len(samples) > 0)
            self.assertIn('rss_mb', samples[0])
            self.assertIn('vms_mb', samples[0])

    @patch('hvm_memory_soak.subprocess.Popen')
    @patch('hvm_memory_soak.subprocess.run')
    def test_scenario_execution(self, mock_run, mock_popen):
        mock_process = MagicMock()
        mock_process.returncode = 0
        mock_process.communicate.return_value = ("Loaded own home from server", "")
        mock_popen.return_value = mock_process

        with tempfile.TemporaryDirectory() as d:
            out_dir = Path(d)
            scenario = hvm_memory_soak.ClashRoyaleScenario(Path("hvm"), Path("app.ipa"), out_dir, 10.0)

            (out_dir / "cr_session").mkdir(parents=True, exist_ok=True)

            with patch('hvm_memory_soak.time.sleep'):
                def side_effect(self, *args, **kwargs):
                    if self.name == "peer.log":
                        return '"event": "listening"'
                    elif self.name == "protocol-events.json":
                        return '{"events": [{"event": "npc_requested", "npc": true}]}'
                    return original_read_text(self, *args, **kwargs)
                original_read_text = Path.read_text

                with patch.object(Path, 'read_text', autospec=True, side_effect=side_effect):
                    mock_process.poll.return_value = None
                    scenario.setup()

            (out_dir / "cr_session" / "protocol-events.json").write_text('{"events": [{"event": "npc_requested", "npc": true}]}')
            status = scenario.execute_cycle(1)
            scenario.teardown()

            self.assertEqual(status, "unsupported")
            self.assertIn("cycle_1_launch", scenario.stages_completed)
            self.assertIn("cycle_1_home", scenario.stages_completed)
            self.assertIn("cycle_1_npc_request", scenario.stages_completed)
            self.assertIn("cycle_1_battle_start_unsupported", scenario.stages_completed)
            self.assertIn("cycle_1_battle_end_unsupported", scenario.stages_completed)

    @patch('hvm_memory_soak.ClashRoyaleScenario.setup')
    @patch('hvm_memory_soak.ClashRoyaleScenario.execute_cycle')
    @patch('hvm_memory_soak.ClashRoyaleScenario.teardown')
    def test_main_loop_unsupported_continues(self, mock_td, mock_exec, mock_su):
        mock_exec.return_value = "unsupported"

        with tempfile.TemporaryDirectory() as d:
            sys.argv = ["hvm_memory_soak.py", "--app", "dummy.ipa", "--output", d, "--battles", "3"]
            rc = hvm_memory_soak.main()
            self.assertEqual(rc, 3)

            subdirs = list(Path(d).glob("soak-*"))
            self.assertEqual(len(subdirs), 1)

            summary = json.loads((subdirs[0] / "summary.json").read_text())
            self.assertEqual(summary["status"], "UNSUPPORTED/PARTIAL")
            self.assertEqual(summary["attempted_cycles"], 3)
            self.assertEqual(mock_exec.call_count, 3)

    @patch('hvm_memory_soak.ClashRoyaleScenario.setup')
    @patch('hvm_memory_soak.ClashRoyaleScenario.execute_cycle')
    @patch('hvm_memory_soak.ClashRoyaleScenario.teardown')
    def test_main_loop_memory_budget_exceeded(self, mock_td, mock_exec, mock_su):
        mock_exec.return_value = "unsupported"

        class MockTelemetry:
            def get_samples(self):
                return [{"rss_mb": 500.0, "vms_mb": 600.0, "stage": "running"}]

        def mock_exec_cycle(cycle):
            # Inject mock telemetry into the scenario during execute_cycle
            import sys
            scenario = sys.modules['hvm_memory_soak'].ClashRoyaleScenario(Path("hvm"), Path("app.ipa"), Path("out"), 10.0)
            scenario.telemetry = MockTelemetry()
            # This is tricky because execute_cycle is mocked, we need to actually set it in the real instance
            return "unsupported"

        mock_exec.side_effect = mock_exec_cycle

        # actually a better way is to mock get_samples since we mock execute_cycle which normally sets it up. But the test just needs samples returned.
        with patch('hvm_memory_soak.ClashRoyaleScenario') as mock_scenario_class:
            mock_inst = mock_scenario_class.return_value
            mock_inst.execute_cycle.return_value = "unsupported"
            mock_telemetry = MagicMock()
            mock_telemetry.get_samples.return_value = [{"rss_mb": 500.0, "vms_mb": 600.0, "stage": "running"}]
            mock_inst.telemetry = mock_telemetry

            with tempfile.TemporaryDirectory() as d:
                sys.argv = ["hvm_memory_soak.py", "--app", "dummy.ipa", "--output", d, "--battles", "3", "--memory-budget", "100"]
                rc = hvm_memory_soak.main()
                self.assertEqual(rc, 1) # MEMORY LIMIT EXCEEDED returns 1

                subdirs = list(Path(d).glob("soak-*"))
                summary = json.loads((subdirs[0] / "summary.json").read_text())
                self.assertEqual(summary["status"], "FAIL - MEMORY_LIMIT")

if __name__ == '__main__':
    unittest.main()
