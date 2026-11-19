#!/usr/bin/env python3
import copy
import json
from pathlib import Path
import tempfile
import unittest

from clash_home_state import encode_home, initial_state, load_state, write_state
from clash_wire import Reader, Writer


def template():
    return {'decks': [[{'d': data, 't': 0, 's': 0, 'l': 0, 'c': 1}
                       for data in (26000000, 26000001, 26000013, 28000001, 28000000, 26000003)]],
            'collection': {'spells': [], 'next_serial': 1}, 'selected_deck': 0,
            'locations': [], 'gambleCounters': [], 'chest_slots': 4,
            'free_chest_t': {'ticks': 288000, 'remaining': 0, 'timestamp': -1},
            'star_chest_cooldown': True}


class HomeStateTests(unittest.TestCase):
    def test_bit_packing_and_alignment_against_literal_bytes(self):
        w = Writer()
        for flag in (True, False, True, True, False, False, False, True, True):
            w.boolean(flag)
        w.vint(64)
        w.boolean(True)
        w.string('é')
        w.reference(18000000)
        self.assertEqual(bytes(w.data), bytes.fromhex('8d0180010100000002c3a91200'))
        r = Reader(bytes(w.data))
        self.assertEqual([r.boolean() for _ in range(9)], [True,False,True,True,False,False,False,True,True])
        self.assertEqual(r.vint(), 64)
        self.assertTrue(r.boolean())
        self.assertEqual(r.string(), 'é')
        self.assertEqual(r.reference(), 18000000)
        self.assertEqual(r.offset, len(r.data))

    def test_initial_wire_shape_and_original_template_preserved(self):
        source = template()
        state = initial_state(source, [0, 1])
        packet = encode_home(state, [0, 1])
        self.assertEqual(len(packet), 184)
        self.assertEqual(packet[:8], bytes.fromhex('0000000000000001'))
        r = Reader(packet)
        self.assertEqual([r.integer(),r.integer()], [0,1])
        self.assertEqual([r.vint() for _ in range(7)], [0,0,0,0,-1,0,1])
        self.assertEqual([r.boolean() for _ in range(8)], [True]*6+[False]*2)
        for data in (26000000,26000001,26000013,28000001,28000000,26000003):
            self.assertEqual(r.reference(), data)
            self.assertEqual([r.vint() for _ in range(5)], [0,0,1,0,0])
            self.assertEqual([r.boolean(),r.boolean()], [False,False])
        self.assertEqual(r.offset, 64)
        self.assertEqual([r.vint() for _ in range(4)], [0,1,0,4])
        self.assertEqual([r.boolean() for _ in range(4)], [False]*4)
        self.assertEqual([r.vint() for _ in range(3)], [288000,0,-1])
        self.assertEqual([r.vint() for _ in range(3)], [0,0,-1])  # donation cooldown
        self.assertFalse(r.boolean());self.assertFalse(r.boolean())
        self.assertEqual(r.vint(),0);self.assertTrue(r.boolean())
        self.assertEqual([r.vint() for _ in range(3)], [0,0,-1])
        self.assertFalse(r.boolean())
        self.assertEqual([r.vint() for _ in range(3)], [0,0,-1])
        self.assertEqual([r.vint() for _ in range(9)], [0]*9)
        self.assertEqual(r.vint(),1)  # last shown level matches the supplied level-one avatar
        state['home']['decks'][0][0]['c'] = 8
        self.assertEqual(source['decks'][0][0]['c'], 1)

    def test_saved_name_and_resources_round_trip_atomically(self):
        state = initial_state(template(), [0,1])
        state['avatar']['name'] = 'Local Player'
        state['avatar']['resources'][0]['count'] = 125
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder)/'home.json'
            write_state(path,state)
            loaded = json.loads(path.read_text())
            self.assertEqual(encode_home(loaded,[0,1]),encode_home(state,[0,1]))
            state['avatar']['diamonds'] = 75
            write_state(path,state)
            self.assertEqual(json.loads(path.read_text())['avatar']['diamonds'],75)
            self.assertEqual(list(Path(folder).iterdir()),[path])

    def test_avatar_shape_and_own_home_tail_are_fully_consumed(self):
        packet = encode_home(initial_state(template(),[0,1]),[0,1])
        r = Reader(packet)
        r.read(123)  # independently derived home section extent for this fixture
        self.assertEqual([r.vint() for _ in range(6)], [0,1]*3)
        self.assertEqual(r.string(),'Offline Player')
        self.assertFalse(r.boolean())
        self.assertEqual(r.vint(),0)
        self.assertEqual(r.reference(),54000000)
        self.assertEqual(r.vint(),0)
        self.assertFalse(r.boolean())
        self.assertEqual(r.vint(),6)
        self.assertEqual(r.vint(),2)
        self.assertEqual([(r.reference(),r.vint()) for _ in range(2)],[(5000001,100),(5000005,100)])
        self.assertEqual([r.vint() for _ in range(5)],[0]*5)
        self.assertEqual([r.vint() for _ in range(5)],[100,100,0,1,0])
        self.assertFalse(r.boolean())
        self.assertEqual([r.vint() for _ in range(6)],[0]*6)
        self.assertFalse(r.boolean())
        self.assertEqual(r.vint(),0)
        self.assertEqual(r.offset,len(packet))

    def test_identity_unsupported_state_and_invalid_decks_rejected(self):
        source = initial_state(template(),[0,1])
        for mutation in (
            lambda s:s.update(account=[0,2]),
            lambda s:s['home'].update(selected_deck=1),
            lambda s:s['home']['decks'][0].append(s['home']['decks'][0][0]),
            lambda s:s['home']['collection']['spells'].append({}),
            lambda s:s['avatar'].update(diamonds=-1),
            lambda s:s['avatar'].update(name='a'*201),
        ):
            state = copy.deepcopy(source)
            mutation(state)
            with self.assertRaises(ValueError):
                encode_home(state,[0,1])

    def test_wrong_client_does_not_create_or_overwrite_state(self):
        with tempfile.TemporaryDirectory() as folder:
            root=Path(folder); ipa=root/'bad.ipa'; ipa.write_bytes(b'wrong client')
            state=root/'home.json'
            with self.assertRaises(ValueError):
                load_state(ipa,state,[0,1])
            self.assertFalse(state.exists())
            state.write_text('existing save')
            with self.assertRaises(ValueError):
                load_state(ipa,state,[0,1])
            self.assertEqual(state.read_text(),'existing save')


if __name__ == '__main__':
    unittest.main()
