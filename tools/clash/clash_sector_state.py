"""Experimental initial Npc1 sector for the pinned ARMv7 client.

Derived from LogicGameMode::decode 0xd7804, LogicBattle::decode 0x14470,
LogicGameObjectManager::decode 0x55c4c, Character::decode 0x12d0ac,
Summoner::decode 0x10cb24 and their component/deck/avatar decoders.
This initializes a battle; it does not implement turn authority or rewards/saves.

Ported from the October 21, 2026 handoff into ``tools/clash/``. CSV rows are
read from the caller's pinned IPA at runtime. This file embeds no client
bytes, tokens, or save data. The protocol peer calls this module only with
the explicit ``--npc-fixture`` option.
"""
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[1]
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

import copy
import csv
import hashlib
import io
import struct
import zipfile

from clash_home_state import PINNED_SHA256, encode_avatar
from clash_wire import Writer


def load_template(ipa):
    with open(ipa, 'rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != PINNED_SHA256:
            raise ValueError('Sector fixture requires the unchanged pinned IPA')
    with zipfile.ZipFile(ipa) as archive:
        roots = [n[:-len('res/level/starting_home.json')] for n in archive.namelist() if n.endswith('.app/res/level/starting_home.json')]
        if len(roots) != 1:
            raise ValueError('Expected one pinned app bundle')

        def rows(path):
            return list(csv.reader(io.StringIO(archive.read(roots[0] + 'res/' + path).decode('utf-8-sig'))))

        def table(name):
            raw = rows('csv_logic/' + name + '.csv')
            return [dict(zip(raw[0], row)) for row in raw[2:]]

        npcs = [r for r in table('npcs') if r.get('Name')]
        npc = npcs[0]
        if (npc['Name'], npc['Location'], npc['PredefinedDeck']) != ('Npc1', 'FirstMatch', 'BasicDeck1'):
            raise ValueError('Unsupported initial mission')
        spells = {}
        for table_id, name in ((26, 'spells_characters'), (27, 'spells_buildings'), (28, 'spells_other')):
            for index, row in enumerate(r for r in table(name) if r.get('Name')):
                spells[row['Name']] = table_id * 1000000 + index
        deck = []
        selected = False
        for row in table('predefined_decks'):
            if row.get('Name'):
                selected = row['Name'] == npc['PredefinedDeck']
            if selected and row.get('Spells'):
                deck.append({'d': spells[row['Spells']], 'l': int(row['SpellLevel']) - 1, 's': 0, 'c': 0, 't': 0})
        buildings = {r['Name']: (35000000 + i, r) for i, r in enumerate(r for r in table('buildings') if r.get('Name'))}
        towers = []
        group = None
        deco_ids = {r['Name']: 42000000 + i for i, r in enumerate(r for r in table('decos') if r.get('Name'))}
        decos = []
        objects = False
        for row in rows('locations/training_arena.csv'):
            if row and row[0] == 'Objects':
                objects = True
                continue
            if not objects or len(row) < 4:
                continue
            if row[1]:
                group = row[1]
            try:
                x, y = int(row[2]), int(row[3])
            except ValueError:
                continue
            if group in deco_ids:
                decos.append(deco_ids[group])
                continue
            if group not in ('KingTower', 'PrincessTower'):
                continue
            data, building = buildings[group]
            owner = 0 if y < 32 else 1  # native world origin is at the bottom
            hp = int(building['Hitpoints'])
            if owner == 0:
                hp = (hp * (int(npc['WizardHpMultiplier'] or 100)) + 99) // 100
            towers.append({'data': data, 'owner': owner, 'x': x * 500, 'y': y * 500, 'hp': hp, 'king': group == 'KingTower'})
        damage_types = [r for r in table('damage_types') if r.get('Name')]
        if len(towers) != 6 or len(deck) != 1 or len(damage_types) != 5:
            raise ValueError('Unsupported initial sector topology')
        return {'npc': 18000000, 'location': 15000000, 'arena': 54000000, 'seed': int(npc['Seed']),
                'npcMana': int(npc['StartingMana']), 'npcDeck': deck, 'towers': towers, 'damageTypes': len(damage_types), 'decos': decos}


def encode_deck(w, deck):
    if not 1 <= len(deck) <= 8:
        raise ValueError('Invalid battle deck')
    for slot in range(8):
        w.boolean(slot < len(deck))
    for card in deck:
        w.reference(card['d'])
        for value in (card['l'], card['s'], card['c'], card['t'], 0):
            w.vint(value)
        w.boolean(False)
        w.boolean(False)


def encode_sector(template, state, account, npc=18000000):
    if npc != 18000000 or template['npc'] != npc:
        raise ValueError('Only the first native NPC mission is supported')
    if state['account'] != list(account) or state['clientSha256'] != PINNED_SHA256:
        raise ValueError('Sector identity/client mismatch')
    if state['avatar']['expLevel'] != 1:
        raise ValueError('Only the initial level-one battle fixture is supported')
    deck = state['home']['decks'][state['home']['selected_deck']]
    decks = (template['npcDeck'], deck)
    towers = template['towers']
    w = Writer()

    def ints(*values):
        for value in values:
            w.vint(value)

    ints(0, 0, 11, 0)
    w.raw(struct.pack('>i', template['seed']))
    ints(0)
    for field in ('location', 'npc', 'arena'):
        w.reference(template[field])
    ints(-1, 0, *account)  # native NPC code expects the human in slot 1
    ints(0, 0)
    ints(*([0] * 8))
    ints(1, 0, 0)  # NPC battle type and constructor counters
    for _ in range(6):
        w.boolean(False)
    decos = template.get('decos', [])
    ints(len(decos), 0, 0, 0, 0, 6)
    ints(len(decos) + 6)  # preserve native static decos in the full update
    for data in decos:
        w.reference(data)
    for tower in towers:
        w.reference(tower['data'])
    for index in range(len(decos)):
        w.reference(1000000 + index)
    for index in range(6):
        w.reference(6000000 + index)  # class ID is type+1
    for tower in towers:
        ints(0)
        w.boolean(False)
        w.boolean(False)  # level index, character flags
        ints(tower['owner'], 13, tower['x'], tower['y'], 0)  # owner, component mask, position, object+30
        ints(0, 256 if tower['owner'] == 0 else -256)  # original spawnObject facing
        ints(0, 100, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)  # character+5c,6c..94,68,64,58
        if tower['king']:
            # KingTower is LogicSummoner, not just LogicCharacter. Its private
            # state has hand indices, draw/discard piles and mana timers.
            hand = min(4, len(decks[tower['owner']]))
            w.boolean(True)
            ints(hand)
            ints(*(i if i < hand else -1 for i in range(4)))
            draw = list(range(hand, len(decks[tower['owner']])))
            ints(len(draw), *draw, 0, -1, 0, 0)
            ints(0, 5 if tower['owner'] == 1 else template['npcMana'], 0, 0)
    # Component passes are by component type, across all objects.
    for _ in towers:  # combat, type 0 (stationary towers have no type-1 movement)
        w.boolean(False)
        w.boolean(False)
        ints(0, 0, 0, 0, 0, 0, 0)
        w.reference(0)
    for tower in towers:
        ints(tower['hp'], tower['hp'], 0)  # hitpoints, type 2
    for _ in towers:  # buff component, type 3; fixed arrays follow damage_types.csv
        ints(0, 0, 0, 0, 0)
        ints(*([0] * template['damageTypes']))
        for _ in range(template['damageTypes']):
            w.boolean(False)
        # Native ctor 0xf164: neutral damage (+0x34) and size (+0x44)
        # percentages are 100; zero shrinks occupants/max HP to 1%.
        ints(100, 0, 0, 0, 100, 0)
    for cards in decks:
        w.boolean(True)
        encode_deck(w, cards)
    for owner in range(2):
        king = next(i for i, t in enumerate(towers) if t['king'] and t['owner'] == owner)
        w.reference(6000000 + king)
    for owner in range(2):
        princess = [i for i, t in enumerate(towers) if not t['king'] and t['owner'] == owner]
        ints(len(princess))
        for index in princess:
            w.reference(6000000 + index)
    ints(0, 0, 0, 0, 0, 0)  # battle+98..ac
    opponent = copy.deepcopy(state['avatar'])
    opponent['name'] = 'Trainer Earl'
    encode_avatar(w, opponent, (-1, 0))
    encode_avatar(w, state['avatar'], account)
    ints(12)
    w.reference(48000000)
    ints(3000)
    w.reference(0)  # canonical NPC tutorial ID, original wait (milliseconds)
    ints(0, 0)  # trailing mode field, queued command count
    return bytes(w.data)
