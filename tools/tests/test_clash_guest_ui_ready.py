"""No UI input should follow a loading-only frame when readiness is requested."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from clash_guest_ui_probe import wait_for_log


class ReadinessTests(unittest.TestCase):
    def test_loading_log_without_ready_message_times_out(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'run.log';path.write_text('Loading SC ui.sc\n')
            with self.assertRaisesRegex(RuntimeError,'readiness log'):
                wait_for_log(path,'Own home queued',0,lambda _:None)

    def test_ready_message_across_read_boundary_is_detected(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'run.log';path.write_text('x'*65532+'Own home queued\n')
            wait_for_log(path,'Own home queued',1,lambda _:self.fail('Already ready'))

    def test_append_after_first_poll_and_app_exit_before_ready(self):
        with tempfile.TemporaryDirectory() as folder:
            path=Path(folder)/'run.log';path.write_text('loading\n')
            def publish(_):
                with path.open('a') as f:f.write('Own home queued\n')
            wait_for_log(path,'Own home queued',1,publish)
            def stopped(_):raise RuntimeError('App exited during UI plan')
            with self.assertRaisesRegex(RuntimeError,'App exited'):
                wait_for_log(path,'Never queued',1,stopped)
