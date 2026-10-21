#!/usr/bin/env python3
"""Check a packaged HydrogenVM device build; does not verify signing or launchability."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import plistlib
import struct
import zipfile


def check_device_executable(data):
    if len(data) < 32:
        raise ValueError('Truncated Mach-O header')
    magic, cpu, _, file_type, count, command_bytes, _, _ = struct.unpack_from('<8I', data)
    if magic != 0xFEEDFACF or cpu != 0x0100000C or file_type != 2:
        raise ValueError('Expected a thin arm64 Mach-O executable for the device build')
    limit = 32 + command_bytes
    if limit > len(data) or count > command_bytes // 8:
        raise ValueError('Truncated Mach-O load commands')
    position = 32
    platforms = []
    for _ in range(count):
        if position + 8 > limit:
            raise ValueError('Truncated load command')
        command, size = struct.unpack_from('<II', data, position)
        if size < 8 or size % 8 or position + size > limit:
            raise ValueError('Invalid load-command length')
        if command == 0x32:  # LC_BUILD_VERSION
            if size < 24:
                raise ValueError('Truncated build-version command')
            platform, _, _, tools = struct.unpack_from('<4I', data, position + 8)
            if 24 + tools * 8 > size:
                raise ValueError('Truncated build-tool list')
            platforms.append(platform)
        if command in (0x21, 0x2C):  # LC_ENCRYPTION_INFO / _64
            if size < 24 or struct.unpack_from('<I', data, position + 16)[0]:
                raise ValueError('Encrypted or malformed device executable')
        position += size
    if position != limit or platforms != [2]:  # PLATFORM_IOS, not PLATFORM_IOSSIMULATOR (7)
        raise ValueError('Expected an iOS device target; simulator/macOS/missing platform rejected')


def verify(ipa, source_commit=None):
    ipa = Path(ipa)
    with zipfile.ZipFile(ipa) as archive:
        members = archive.infolist()
        names = [member.filename for member in members]
        if len(names) != len(set(names)):
            raise ValueError('Duplicate archive entries')
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or '\\' in name:
                raise ValueError('Invalid archive path')
        bad = archive.testzip()
        if bad:
            raise ValueError(f'Archive CRC failure: {bad}')
        infos = [name for name in names if name.startswith('Payload/') and len(name.split('/')) == 3 and name.endswith('.app/Info.plist')]
        if infos != ['Payload/HydrogenVM.app/Info.plist']:
            raise ValueError('Expected exactly Payload/HydrogenVM.app/Info.plist')
        info = plistlib.loads(archive.read(infos[0]))
        if info.get('CFBundleIdentifier') != 'com.lfull.HydrogenVM' or info.get('CFBundlePackageType') != 'APPL':
            raise ValueError('Wrong app identifier or package type')
        for key in ('CFBundleVersion', 'CFBundleShortVersionString', 'MinimumOSVersion'):
            if not isinstance(info.get(key), str) or not info[key] or '$(' in info[key]:
                raise ValueError(f'Missing/unexpanded metadata: {key}')
        if info.get('CFBundleSupportedPlatforms') != ['iPhoneOS']:
            raise ValueError('Info.plist does not identify an iPhoneOS device build')
        executable = info.get('CFBundleExecutable', '')
        if not executable or PurePosixPath(executable).name != executable or executable in ('.', '..'):
            raise ValueError('Invalid executable name')
        member = archive.getinfo('Payload/HydrogenVM.app/' + executable)
        if not (member.external_attr >> 16) & 0o111:
            raise ValueError('Executable permission bits were lost during packaging')
        check_device_executable(archive.read(member))
        with ipa.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        return {
            'ipa': ipa.name, 'sha256': digest, 'bytes': ipa.stat().st_size,
            'bundleIdentifier': info['CFBundleIdentifier'], 'version': info['CFBundleShortVersionString'],
            'build': info['CFBundleVersion'], 'minimumOSVersion': info['MinimumOSVersion'],
            'architecture': 'arm64', 'platform': 'iOS device', 'sourceCommit': source_commit,
            'archiveChecksPassed': True, 'signatureVerified': False, 'deviceTested': False,
        }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('ipa', type=Path)
    parser.add_argument('--out', type=Path)
    parser.add_argument('--source-commit')
    args = parser.parse_args()
    try:
        result = verify(args.ipa, args.source_commit)
    except (ValueError, KeyError, OSError, zipfile.BadZipFile, plistlib.InvalidFileException) as error:
        parser.exit(1, f'IPA validation failed: {error}\n')
    text = json.dumps(result, indent=2) + '\n'
    if args.out:
        args.out.write_text(text)
        args.out.with_suffix('.sha256').write_text(f"{result['sha256']}  {args.ipa.name}\n")
    print(text, end='')


if __name__ == '__main__':
    main()
