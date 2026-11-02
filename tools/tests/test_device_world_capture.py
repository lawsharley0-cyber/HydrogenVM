import hashlib
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from device_world_capture import validate_capture


class DeviceWorldCaptureTests(unittest.TestCase):
    def capture(self, root):
        roles = ['session-log', 'screenshot', 'crash-json', 'ips', 'symbols']
        artifacts = []
        for role in roles:
            data = ('synthetic ' + role).encode()
            (root / role).write_bytes(data)
            artifacts.append({'role': role, 'path': role, 'sha256': hashlib.sha256(data).hexdigest()})
        return {'schemaVersion': 1, 'kind': 'hvm-device-world-capture',
                'emulator': {'commit': 'a' * 40, 'build': 'synthetic-test', 'dirty': False},
                'host': {'system': 'iOS', 'deviceModel': 'synthetic', 'osVersion': 'test', 'pageSize': 16384, 'renderer': 'software'},
                'fixture': {'ipaSha256': 'b' * 64, 'bundleId': 'test.synthetic', 'version': '1'},
                'scenario': {'durationSeconds': 61, 'homeState': 'preserved test sandbox',
                             'actions': [{'atSeconds': 2, 'action': 'tap Play'}, {'atSeconds': 4, 'action': 'tap world'}],
                             'outcome': 'host-crash'}, 'artifacts': artifacts}

    def test_complete_capture_is_not_a_compatibility_verdict(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            self.assertEqual(validate_capture(self.capture(root), root), [])

    def test_missing_renderer_is_incomplete(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); doc = self.capture(root)
            del doc['host']['renderer']
            self.assertTrue(any('renderer' in e for e in validate_capture(doc, root)))

    def test_world_loaded_requires_sixty_seconds_after_recorded_entry(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); doc = self.capture(root)
            doc['scenario']['outcome'] = 'world-loaded'
            self.assertTrue(validate_capture(doc, root))
            doc['scenario']['worldEntrySeconds'] = 2
            self.assertTrue(validate_capture(doc, root))
            doc['scenario']['durationSeconds'] = 62
            self.assertEqual(validate_capture(doc, root), [])

    def test_large_integer_times_are_validation_errors_without_tracebacks(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); doc = self.capture(root)
            doc['scenario']['durationSeconds'] = 10 ** 1000
            doc['scenario']['actions'][0]['atSeconds'] = 10 ** 999
            errors = validate_capture(doc, root)
            self.assertTrue(any('durationSeconds' in e for e in errors))
            self.assertTrue(any('timestamp' in e for e in errors))

    def test_distinct_required_roles_cannot_reuse_one_file(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d); doc = self.capture(root)
            first = doc['artifacts'][0]
            for artifact in doc['artifacts']:
                artifact.update(path=first['path'], sha256=first['sha256'])
            self.assertTrue(any('reuse' in e for e in validate_capture(doc, root)))

    def test_boolean_schema_version_is_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            doc['schemaVersion'] = True
            self.assertTrue(any('schemaVersion' in e for e in validate_capture(doc, root)))

    def test_linux_16kb_emulation_does_not_replace_physical_device_evidence(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            doc['host']['system'] = 'Linux'
            self.assertIn('host.system must be iOS', validate_capture(doc, root))

    def test_missing_or_modified_artifact_is_not_complete(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            (root / 'ips').write_text('changed')
            (root / 'crash-json').unlink()
            errors = validate_capture(doc, root)
            self.assertTrue(any('checksum' in e for e in errors))
            self.assertTrue(any('crash-json' in e for e in errors))

    def test_paths_cannot_escape_capture_directory(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            doc['artifacts'][0]['path'] = '../outside'
            self.assertTrue(any('escape' in e for e in validate_capture(doc, root)))

    def test_matching_fixture_and_build_are_required_when_requested(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            errors = validate_capture(doc, root, expected_commit='c' * 40, expected_ipa='d' * 64)
            self.assertIn('emulator commit does not match the requested build', errors)
            self.assertIn('IPA hash does not match the requested fixture', errors)

    def test_invalid_duration_and_missing_fault_evidence_are_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            doc = self.capture(root)
            doc['scenario']['durationSeconds'] = float('nan')
            doc['artifacts'] = [a for a in doc['artifacts'] if a['role'] != 'ips']
            errors = validate_capture(doc, root)
            self.assertTrue(any('durationSeconds' in e for e in errors))
            self.assertTrue(any('ips' in e for e in errors))


if __name__ == '__main__':
    unittest.main()
