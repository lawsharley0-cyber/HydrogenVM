#!/usr/bin/env python3
"""Validate completeness/integrity of a private physical-device capture.

This checks metadata and artifact hashes, not whether a game is compatible or
whether a supplied outcome is true. It never uploads artifacts or substitutes
a Linux run for an iPhone run.
"""
import argparse
import hashlib
import json
import math
from pathlib import Path, PureWindowsPath
import re


def sha256(path):
    digest = hashlib.sha256()
    with path.open('rb') as stream:
        while block := stream.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def validate_capture(doc, root, expected_commit=None, expected_ipa=None):
    errors = []
    root = Path(root).resolve()
    if not isinstance(doc, dict):
        return ['capture must be a JSON object']
    if type(doc.get('schemaVersion')) is not int or doc.get('schemaVersion') != 1 or doc.get('kind') != 'hvm-device-world-capture':
        errors.append('unsupported capture kind/schemaVersion')
    sections = {}
    for key in ('emulator', 'host', 'fixture', 'scenario'):
        value = doc.get(key)
        if not isinstance(value, dict):
            errors.append(f'{key} must be an object')
            value = {}
        sections[key] = value
    emulator, host, fixture, scenario = (sections[k] for k in ('emulator', 'host', 'fixture', 'scenario'))
    for obj, keys, prefix in ((emulator, ('build',), 'emulator'),
                              (host, ('deviceModel', 'osVersion'), 'host'),
                              (fixture, ('bundleId', 'version'), 'fixture'),
                              (scenario, ('homeState',), 'scenario')):
        for key in keys:
            if not isinstance(obj.get(key), str) or not obj[key].strip():
                errors.append(f'{prefix}.{key} is required')
    commit, ipa = emulator.get('commit'), fixture.get('ipaSha256')
    if not isinstance(commit, str) or not re.fullmatch(r'[0-9a-fA-F]{40}', commit):
        errors.append('emulator.commit must be a full Git SHA')
    if type(emulator.get('dirty')) is not bool:
        errors.append('emulator.dirty must be recorded explicitly')
    if not isinstance(ipa, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', ipa):
        errors.append('fixture.ipaSha256 must identify the original private IPA')
    if expected_commit and str(commit).lower() != expected_commit.lower():
        errors.append('emulator commit does not match the requested build')
    if expected_ipa and str(ipa).lower() != expected_ipa.lower():
        errors.append('IPA hash does not match the requested fixture')
    if host.get('system') != 'iOS':
        errors.append('host.system must be iOS')
    page = host.get('pageSize')
    if type(page) is not int or page < 4096 or page & (page - 1):
        errors.append('host.pageSize must be the measured native page size')
    duration = scenario.get('durationSeconds')
    finite_duration = type(duration) in (int, float) and math.isfinite(duration) and duration >= 0
    if not finite_duration:
        errors.append('scenario.durationSeconds must be finite and nonnegative')
    actions = scenario.get('actions')
    if not isinstance(actions, list) or not actions:
        errors.append('scenario.actions must record the world-load sequence')
    else:
        previous = -1
        for index, action in enumerate(actions):
            at = action.get('atSeconds') if isinstance(action, dict) else None
            if type(at) not in (int, float) or not math.isfinite(at) or at < previous or at < 0:
                errors.append(f'scenario action {index} has an invalid timestamp')
                continue
            previous = at
            if finite_duration and at > duration:
                errors.append(f'scenario action {index} occurs after capture ended')
            if not isinstance(action.get('action'), str) or not action['action'].strip():
                errors.append(f'scenario action {index} must describe the actual input')
    outcome = scenario.get('outcome')
    required = {'session-log', 'screenshot'}
    if outcome == 'host-crash':
        required |= {'crash-json', 'ips', 'symbols'}
    elif outcome == 'guest-crash':
        required |= {'crash-json'}
    elif outcome in ('jetsam', 'watchdog'):
        required |= {'ips'}
    elif outcome not in ('world-loaded', 'unconfirmed'):
        errors.append('scenario.outcome must distinguish host/guest crash, jetsam, watchdog, world-loaded or unconfirmed')
    artifacts = doc.get('artifacts')
    roles = set()
    if not isinstance(artifacts, list):
        errors.append('artifacts must be a list')
        artifacts = []
    for index, artifact in enumerate(artifacts):
        if not isinstance(artifact, dict):
            errors.append(f'artifact {index} must be an object')
            continue
        role, relative = artifact.get('role'), artifact.get('path')
        if not isinstance(role, str) or not isinstance(relative, str):
            errors.append(f'artifact {index} requires a role and relative path')
            continue
        roles.add(role)
        rel = Path(relative)
        path = (root / rel).resolve()
        if rel.is_absolute() or PureWindowsPath(relative).is_absolute() or '..' in rel.parts or not path.is_relative_to(root):
            errors.append(f'artifact {role} path may not escape the capture directory')
            continue
        digest = artifact.get('sha256')
        if not isinstance(digest, str) or not re.fullmatch(r'[0-9a-fA-F]{64}', digest):
            errors.append(f'artifact {role} requires a SHA-256 checksum')
            continue
        try:
            if not path.is_file() or path.stat().st_size == 0:
                errors.append(f'artifact {role} is missing or empty')
            elif sha256(path) != digest.lower():
                errors.append(f'artifact {role} checksum does not match')
        except OSError as error:
            errors.append(f'artifact {role} cannot be read: {error}')
    for missing in sorted(required - roles):
        errors.append(f'required artifact role missing: {missing}')
    return errors


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('capture', type=Path)
    parser.add_argument('--expected-commit')
    parser.add_argument('--expected-ipa-sha256')
    args = parser.parse_args(argv)
    try:
        doc = json.loads(args.capture.read_text(encoding='utf-8'))
        errors = validate_capture(doc, args.capture.parent, args.expected_commit, args.expected_ipa_sha256)
    except (OSError, ValueError) as error:
        errors = [str(error)]
    print(json.dumps({'captureComplete': not errors, 'compatibilityVerdict': 'unverified', 'errors': errors}, indent=2))
    return 1 if errors else 0


if __name__ == '__main__':
    raise SystemExit(main())
