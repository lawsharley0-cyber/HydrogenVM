"""Authored CSV fixtures: stable original IDs, independent clones, safe registry growth."""
import csv
import io
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT/'tools'))
from clash_custom_cards import generate_overlays, validate_registry, registry_identity, require_registry_identity


class CustomDefinitionTests(unittest.TestCase):
    def fixtures(self, root):
        definitions = {
            'logic/characters': [['Name', 'Projectile', 'Hitpoints', 'TID'], ['Knight', '', '100', 'OLD_K'], ['Archer', 'Arrow', '60', 'OLD_A'], ['', '', '70', '']],
            'logic/buildings': [['Name', 'Projectile', 'TID'], ['Cannon', 'CannonBall', 'OLD_C']],
            'logic/projectiles': [['Name', 'Speed', 'Damage'], ['Arrow', '300', '40'], ['CannonBall', '200', '50']],
            'logic/spells_characters': [['Name', 'SummonCharacter', 'NotInUse', 'TID', 'IconFile'], ['Knight', 'Knight', '', 'OLD_K', 'knight'], ['Archers', 'Archer', '', 'OLD_A', 'archer']],
            'logic/spells_buildings': [['Name', 'SummonCharacter', 'NotInUse', 'TID', 'IconFile'], ['CannonCard', 'Cannon', '', 'OLD_C', 'cannon']],
            'client/texts': [['TID', 'EN', 'FR'], ['OLD_K', 'Knight', 'Chevalier']]
        }
        for name, rows in definitions.items():
            path = root/f'res/csv_{name}.csv'
            path.parent.mkdir(parents=True, exist_ok=True)
            with path.open('w', newline='', encoding='utf-8') as f:
                writer = csv.writer(f)
                writer.writerow(rows[0]); writer.writerow(['string'] * len(rows[0])); writer.writerows(rows[1:])

    def registry(self, base=26000001, name='Ace, "Arrow"'):
        return dict(version=1, cards=[dict(slot=0, baseCardId=base, name=name)])

    def test_clone_references_and_original_prefix_are_independent(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); self.fixtures(root)
            original = {str(p.relative_to(root)): p.read_bytes() for p in root.rglob('*.csv')}
            overlays, meta = generate_overlays(root, self.registry())
            self.assertEqual(meta['cards'][0]['cardId'], 26000002)
            for path, data in overlays.items():
                self.assertTrue(data.startswith(original[path]), path)
                self.assertEqual((root/path).read_bytes(), original[path])
            rows = lambda name: list(csv.reader(io.StringIO(overlays[f'res/csv_{name}.csv'].decode())))
            spell = rows('logic/spells_characters')[-1]
            self.assertEqual(spell, ['ModLabCard_0000', 'ModLabUnit_0000', 'true', 'TID_MODLAB_0000', 'archer'])
            unit = rows('logic/characters')[-2]
            self.assertEqual(unit, ['ModLabUnit_0000', 'ModLabShot_0000', '60', 'TID_MODLAB_0000'])
            self.assertEqual(rows('logic/characters')[-1], ['', '', '70', ''])
            self.assertEqual(rows('logic/projectiles')[-1], ['ModLabShot_0000', '300', '40'])
            self.assertEqual(rows('client/texts')[-1], ['TID_MODLAB_0000', 'Ace, "Arrow"', 'Ace, "Arrow"'])

    def test_mixed_registry_has_separate_card_ids_and_projectiles(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.fixtures(root)
            other = root / 'res/csv_logic/spells_other.csv'
            fields = ['Name', 'Projectile', 'AreaEffectObject', 'CustomFirstProjectile',
                      'SummonCharacter', 'Mirror', 'TID', 'NotInUse', 'IconFile']
            with other.open('w', newline='') as stream:
                writer = csv.writer(stream)
                writer.writerows([fields, ['string'] * len(fields),
                                  ['Fireball', 'Arrow', '', '', '', '', 'OLD_F', '', 'icon'],
                                  ['Reserved'], ['ReservedTwo'], ['Rocket', 'Arrow']])
            old = self.registry()
            registry = dict(version=1, cards=old['cards'] + [
                dict(slot=1, baseCardId=28000000, name='Star'),
                dict(slot=2, baseCardId=27000000, name='Tower'),
                dict(slot=3, baseCardId=28000003, name='Comet')])
            overlays, meta = generate_overlays(root, registry, previous=old)
            self.assertEqual([c['cardId'] for c in meta['cards']],
                             [26000002, 28000004, 27000001, 28000005])
            spells = overlays['res/csv_logic/spells_other.csv']
            self.assertTrue(spells.startswith(other.read_bytes()))
            rows = list(csv.reader(io.StringIO(spells.decode())))
            self.assertEqual(rows[-2], ['ModLabCard_0001', 'ModLabShot_0001', '', '', '', '',
                                         'TID_MODLAB_0001', 'true', 'icon'])
            self.assertEqual(rows[-1][1], 'ModLabShot_0003')
            shots = list(csv.reader(io.StringIO(overlays['res/csv_logic/projectiles.csv'].decode())))
            self.assertEqual([row for row in shots if row[:1] == ['ModLabShot_0001']],
                             [['ModLabShot_0001', '300', '40']])
            self.assertEqual(meta['cards'][1]['unit'], '')

    def test_append_only_ids_span_native_tables_without_reassigning(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); self.fixtures(root)
            old = self.registry(); new = dict(version=1, cards=old['cards'] + [dict(slot=1, baseCardId=27000000, name='Custom Cannon'), dict(slot=2, baseCardId=26000000, name='Custom Knight')])
            _, meta = generate_overlays(root, new, old)
            self.assertEqual([c['cardId'] for c in meta['cards']], [26000002, 27000001, 26000003])
            self.assertEqual(meta['cards'][2]['projectile'], '')
            for changed in [dict(version=1, cards=[]), self.registry(name='Reassigned')]:
                with self.assertRaises(ValueError): validate_registry(changed, old)

    def test_invalid_schema_slots_ids_names_and_card_limit_reject(self):
        for invalid in [dict(version=True, cards=[]), dict(version=1, cards=[], extra=0), self.registry(base=True), self.registry(base=28000006), self.registry(name=''), self.registry(name='a\nb'), self.registry(name='a'*33), dict(version=1, cards=[dict(slot=1, baseCardId=26000000, name='Gap')]), dict(version=1, cards=[dict(slot=i, baseCardId=26000000, name='Too many') for i in range(17)])]:
            with self.subTest(registry=invalid), self.assertRaises(ValueError): validate_registry(invalid)
        for separator in ['\x85', '\u2028', '\u2029']:
            with self.subTest(separator=separator), self.assertRaises(ValueError):
                validate_registry(self.registry(name=f'First{separator}second'))
        validate_registry(self.registry(name='勇敢な Knight 👩🏽\u200d🚀'))

    def test_unknown_source_ids_and_missing_projectiles_reject(self):
        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder); self.fixtures(root)
            with self.assertRaises(ValueError): generate_overlays(root, self.registry(base=26000099))
            path = root/'res/csv_logic/projectiles.csv'
            path.write_text('Name,Speed,Damage\nstring,string,string\nOther,1,1\n')
            with self.assertRaises(ValueError): generate_overlays(root, self.registry())

    def test_saved_definition_identity_survives_pending_append_only(self):
        old = self.registry()
        required = registry_identity(old)
        appended = dict(version=1, cards=old['cards'] + [dict(slot=1, baseCardId=27000000, name='Next card')])
        require_registry_identity(appended, required)
        self.assertNotEqual(registry_identity(appended), required)
        self.assertEqual(registry_identity(appended, 1), required)
        for changed in [dict(version=1, cards=[]), self.registry(name='Renamed'), self.registry(base=26000000)]:
            with self.subTest(registry=changed), self.assertRaises(ValueError):
                require_registry_identity(changed, required)

    def test_invalid_saved_identity_is_not_treated_as_no_custom_cards(self):
        registry = self.registry()
        for invalid in [None, {}, dict(count=True, sha256='0'*64), dict(count=-1, sha256='0'*64),
                        dict(count=2, sha256='0'*64), dict(count=1, sha256='x'*64),
                        dict(count=1, sha256=registry_identity(registry)['sha256'], owned=True)]:
            with self.subTest(required=invalid), self.assertRaises(ValueError):
                require_registry_identity(registry, invalid)
        empty = dict(version=1, cards=[])
        require_registry_identity(registry, registry_identity(empty))


if __name__ == '__main__': unittest.main()
