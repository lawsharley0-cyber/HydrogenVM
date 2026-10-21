"""Synthetic archive tests exercise rejection gates; no fixture is an installable app."""
from pathlib import Path
import plistlib
import struct
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from verify_ipa import verify


class PackageChecks(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.ipa = Path(self.temp.name) / 'test.ipa'

    def make(self, platform=2, cpu=0x0100000C, permissions=0o100755, identifier='com.lfull.HydrogenVM', encrypted=False):
        info = {'CFBundleIdentifier': identifier, 'CFBundlePackageType': 'APPL', 'CFBundleExecutable': 'HydrogenVM',
                'CFBundleVersion': '1', 'CFBundleShortVersionString': '0.1.0', 'MinimumOSVersion': '26.0',
                'CFBundleSupportedPlatforms': ['iPhoneOS']}
        commands = struct.pack('<6I', 0x32, 24, platform, 26 << 16, 26 << 16, 0)
        if encrypted:
            commands += struct.pack('<6I', 0x2C, 24, 0, 0, 1, 0)
        binary = struct.pack('<8I', 0xFEEDFACF, cpu, 0, 2, 2 if encrypted else 1, len(commands), 0, 0) + commands
        with zipfile.ZipFile(self.ipa, 'w') as archive:
            archive.writestr('Payload/HydrogenVM.app/Info.plist', plistlib.dumps(info))
            entry = zipfile.ZipInfo('Payload/HydrogenVM.app/HydrogenVM')
            entry.external_attr = permissions << 16
            archive.writestr(entry, binary)

    def test_device_metadata_and_hash(self):
        self.make()
        result = verify(self.ipa, 'a' * 40)
        self.assertEqual(result['sourceCommit'], 'a' * 40)
        self.assertEqual(len(result['sha256']), 64)
        self.assertFalse(result['signatureVerified'])
        self.assertFalse(result['deviceTested'])

    def test_arm64_simulator_is_not_a_device_build(self):
        self.make(platform=7)
        with self.assertRaisesRegex(ValueError, 'device target'):
            verify(self.ipa)

    def test_wrong_architecture(self):
        self.make(cpu=12)
        with self.assertRaisesRegex(ValueError, 'arm64'):
            verify(self.ipa)

    def test_lost_executable_mode(self):
        self.make(permissions=0o100644)
        with self.assertRaisesRegex(ValueError, 'permission'):
            verify(self.ipa)

    def test_wrong_app(self):
        self.make(identifier='com.example.OtherApp')
        with self.assertRaisesRegex(ValueError, 'identifier'):
            verify(self.ipa)

    def test_encrypted_binary(self):
        self.make(encrypted=True)
        with self.assertRaisesRegex(ValueError, 'Encrypted'):
            verify(self.ipa)


if __name__ == '__main__':
    unittest.main()
