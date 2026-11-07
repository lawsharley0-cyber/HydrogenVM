"""Build private custom-card CSV overlays without changing bundled assets.

This is definition-generation infrastructure, not an in-game card forge. The
guest still needs a durable registry, loader routing and native ownership/UI.
Inputs must come from the pinned bundle; no client data is distributed here.
"""
import csv
import hashlib
import io
import json
from pathlib import Path

MAX_CUSTOM_CARDS = 16


class Table:
    def __init__(self, bundle, category, name):
        self.path = f'res/csv_{category}/{name}.csv'
        self.original = (Path(bundle)/self.path).read_bytes()
        rows = list(csv.reader(io.StringIO(self.original.decode('utf-8-sig'))))
        if len(rows) < 2 or not rows[0]:
            raise ValueError(f'Invalid CSV header: {self.path}')
        self.header = rows[0]
        self.groups = []
        for row in rows[2:]:
            if row and row[0]:
                if any(g[0][0] == row[0] for g in self.groups):
                    raise ValueError(f'Duplicate CSV name: {self.path}/{row[0]}')
                self.groups.append([row])
            elif self.groups:
                self.groups[-1].append(row)
        self.additions = []

    def get(self, name):
        for group in self.groups:
            if group[0][0] == name:
                return [row[:] for row in group]
        raise ValueError(f'Missing CSV reference: {self.path}/{name}')

    def value(self, row, field):
        at = self.header.index(field)
        return row[at] if at < len(row) else ''

    def set(self, row, field, value):
        at = self.header.index(field)
        row.extend([''] * max(0, at + 1 - len(row)))
        row[at] = value

    def append(self, group):
        name = group[0][0]
        if any(g[0][0] == name for g in self.groups + self.additions):
            raise ValueError(f'Custom name collides with existing data: {name}')
        self.additions.append(group)

    def render(self):
        stream = io.StringIO(newline='')
        writer = csv.writer(stream, lineterminator='\r\n')
        for group in self.additions:
            writer.writerows(group)
        separator = b'' if self.original.endswith(b'\n') else b'\r\n'
        return self.original + separator + stream.getvalue().encode('utf-8')


def validate_registry(registry, previous=None):
    if not isinstance(registry, dict) or set(registry) != {'version', 'cards'} or type(registry['version']) is not int or registry['version'] != 1:
        raise ValueError('Unsupported custom-card registry')
    cards = registry['cards']
    if not isinstance(cards, list) or len(cards) > MAX_CUSTOM_CARDS:
        raise ValueError('Custom-card limit exceeded')
    for slot, card in enumerate(cards):
        if not isinstance(card, dict) or set(card) != {'slot', 'baseCardId', 'name'}:
            raise ValueError('Invalid custom-card record')
        if type(card['slot']) is not int or card['slot'] != slot or type(card['baseCardId']) is not int or card['baseCardId'] // 1000000 not in (26, 27):
            raise ValueError('Slots must be append-only troop/building definitions')
        name = card['name']
        if not isinstance(name, str) or not name.strip() or len(name) > 32 or len(name.encode('utf-8')) > 96 or any(ord(c) < 32 or 127 <= ord(c) <= 159 or c in '\u2028\u2029' for c in name):
            raise ValueError('Custom name must be single-line text, at most 32 characters/96 UTF-8 bytes')
    if previous is not None:
        validate_registry(previous)
        old = previous['cards']
        if len(cards) < len(old) or cards[:len(old)] != old:
            raise ValueError('Existing definitions must not be removed, reordered or reassigned')
    return cards


def registry_identity(registry, count=None):
    """Identify an immutable prefix, so appending a card retains old save links.

    This describes definitions only; ownership and mutable tuning must never
    enter this hash. It is an integrity check, not an authentication mechanism.
    Runtime save loading is not wired to this helper yet.
    """
    cards = validate_registry(registry)
    if count is None:
        count = len(cards)
    if type(count) is not int or not 0 <= count <= len(cards):
        raise ValueError('Required custom definitions are missing')
    prefix = dict(version=1, cards=cards[:count])
    payload = json.dumps(prefix, sort_keys=True, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    return dict(count=count, sha256=hashlib.sha256(payload).hexdigest())


def require_registry_identity(registry, required):
    """Reject missing/reassigned definitions; allow a longer unchanged prefix."""
    if not isinstance(required, dict) or set(required) != {'count', 'sha256'}:
        raise ValueError('Invalid required custom-definition identity')
    digest = required['sha256']
    if not isinstance(digest, str) or len(digest) != 64 or any(c not in '0123456789abcdef' for c in digest):
        raise ValueError('Invalid custom-definition digest')
    if registry_identity(registry, required['count']) != required:
        raise ValueError('Custom definitions do not match the saved collection')


def generate_overlays(bundle, registry, previous=None):
    """Return changed CSV bytes and ID metadata; never write the source bundle.

    New definitions are deliberately NotInUse. Making one owned/playable is a
    separate guest transaction after training, not a natural loot-pool change.
    """
    cards = validate_registry(registry, previous)
    tables = {name: Table(bundle, 'logic', name) for name in
              ('characters', 'buildings', 'spells_characters', 'spells_buildings', 'projectiles')}
    texts = Table(bundle, 'client', 'texts')
    spell_tables = {26: tables['spells_characters'], 27: tables['spells_buildings']}
    metadata = []
    for card in cards:
        kind, index = divmod(card['baseCardId'], 1000000)
        spells = spell_tables[kind]
        if index >= len(spells.groups):
            raise ValueError('Unknown original base card ID')
        spell = [r[:] for r in spells.groups[index]]
        unit_name = spells.value(spell[0], 'SummonCharacter')
        units = tables['characters' if kind == 26 else 'buildings']
        unit = units.get(unit_name)
        suffix = f'{card["slot"]:04d}'
        spell_name, unit_name, tid = f'ModLabCard_{suffix}', f'ModLabUnit_{suffix}', f'TID_MODLAB_{suffix}'
        spell[0][0], unit[0][0] = spell_name, unit_name
        spells.set(spell[0], 'SummonCharacter', unit_name)
        spells.set(spell[0], 'NotInUse', 'true')
        spells.set(spell[0], 'TID', tid)
        units.set(unit[0], 'TID', tid)
        projectile = units.value(unit[0], 'Projectile')
        if projectile:
            shots = tables['projectiles']
            shot = shots.get(projectile)
            projectile = f'ModLabShot_{suffix}'
            shot[0][0] = projectile
            shots.append(shot)
            units.set(unit[0], 'Projectile', projectile)
        custom_id = kind * 1000000 + len(spells.groups) + len(spells.additions)
        spells.append(spell)
        units.append(unit)
        text = [''] * len(texts.header)
        text[0] = tid
        for i, language in enumerate(texts.header[1:], 1):
            if language:
                text[i] = card['name']
        texts.append([text])
        metadata.append(dict(slot=card['slot'], cardId=custom_id, baseCardId=card['baseCardId'],
                             spell=spell_name, unit=unit_name, projectile=projectile, tid=tid))
    changed = {t.path: t.render() for t in [*tables.values(), texts] if t.additions}
    identity = registry_identity(registry)
    return changed, dict(version=1, registrySHA256=identity['sha256'],
                         requiredDefinitions=identity, cards=metadata)
