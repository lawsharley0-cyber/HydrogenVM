"""Initial home/avatar state for the pinned 2.1306.1 client.

Wire order is derived from LogicClientHome::decode (0x124f9c),
LogicClientAvatar::decode (0x9a008), LogicSpell::decode (0x372ec), and
OwnHomeDataMessage::decode (0x1921c). Unnamed members are kept at constructor
defaults and identified by object offsets. This is a startup fixture, not yet
a progression engine, chest economy or battle server.
"""
import copy
import hashlib
import json
import os
from pathlib import Path
import struct
import tempfile
import zipfile

from clash_wire import Writer

PINNED_SHA256 = '29269ede437f39ffa62fb4234a1c323303ef417e7eb6ddc5c86f9be76e08fb3f'
IDLE_TIMER = {'ticks': 0, 'remaining': 0, 'timestamp': -1}


def initial_state(template, account):
    return {'format': 1, 'clientSha256': PINNED_SHA256, 'account': list(account),
            'home': copy.deepcopy(template),
            'avatar': {'name': 'Offline Player', 'nameSet': False, 'arena': 54000000,
                       'diamonds': 100, 'freeDiamonds': 100, 'expPoints': 0, 'expLevel': 1,
                       'resources': [{'data': 5000001, 'count': 100}, {'data': 5000005, 'count': 100}]}}


def write_state(path, state):
    """Atomic replacement in the same directory; an interrupted write keeps the old state."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(prefix=path.name + '.', dir=path.parent)
    try:
        with os.fdopen(fd, 'w') as target:
            json.dump(state, target, indent=2)
            target.write('\n')
            target.flush()
            os.fsync(target.fileno())
        os.replace(temporary, path)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def load_state(ipa, path, account):
    with Path(ipa).open('rb') as source:
        if hashlib.file_digest(source, 'sha256').hexdigest() != PINNED_SHA256:
            raise ValueError('Home fixture requires the unchanged pinned IPA')
    if Path(path).exists():
        state = json.loads(Path(path).read_text())
    else:
        with zipfile.ZipFile(ipa) as archive:
            names = [n for n in archive.namelist() if n.startswith('Payload/') and n.endswith('.app/res/level/starting_home.json')]
            if len(names) != 1:
                raise ValueError('Expected exactly one starting_home.json')
            state = initial_state(json.loads(archive.read(names[0])), account)
        encode_home(state, account)  # validate before creating any state
        write_state(path, state)
    encode_home(state, account)
    return state


def encode_home(state, account):
    if state.get('format') != 1 or state.get('clientSha256') != PINNED_SHA256 or state.get('account') != list(account):
        raise ValueError('Home state identity/client mismatch')
    home, avatar = state['home'], state['avatar']
    decks = home['decks']
    if not 1 <= len(decks) <= 3 or not 0 <= home['selected_deck'] < len(decks):
        raise ValueError('Invalid deck count/selection')
    if home['chest_slots'] != 4 or home['locations'] or home['gambleCounters'] or home['collection']['spells']:
        raise ValueError('Only the initial empty chest/collection/location fixture is supported')
    if not 1 <= avatar['expLevel'] <= 13 or avatar['arena'] != 54000000:
        raise ValueError('Only a training-camp avatar is currently supported')
    w = Writer()
    w.raw(struct.pack('>ii', *account))  # home ID: ByteStream::readLong
    w.vint(0)  # home+bc
    w.vint(0)  # home+13c
    w.timer(IDLE_TIMER)  # donation capacity
    w.vint(0)  # home+b0
    w.vint(len(decks))
    for deck in decks:
        if not 1 <= len(deck) <= 8 or len({c['d'] for c in deck}) != len(deck):
            raise ValueError('Invalid/repeated deck cards')
        for slot in range(8):
            w.boolean(slot < len(deck))
        for card in deck:
            if card['d'] // 1000000 not in (26, 27, 28):
                raise ValueError('Invalid spell data type')
            w.reference(card['d'])
            for value in (card['l'], card['s'], card['c'], card['t'], 0):
                w.vint(value)
            w.boolean(False)  # spell+14
            w.boolean(False)  # spell+15
    w.vint(0)  # collection count
    w.vint(home['collection']['next_serial'])
    w.vint(home['selected_deck'])
    w.vint(home['chest_slots'])
    for _ in range(home['chest_slots']):
        w.boolean(False)
    w.timer(home['free_chest_t'])
    w.timer(IDLE_TIMER)  # donation cooldown
    w.boolean(False)  # free chest object
    w.boolean(False)  # purchased chest object
    w.vint(0)  # crown count
    w.boolean(home['star_chest_cooldown'])
    w.timer(IDLE_TIMER)  # crown chest cooldown
    w.boolean(False)  # crown chest object
    w.timer(IDLE_TIMER)  # card request cooldown
    for _ in range(8):
        w.vint(0)  # home+2c..48
    w.vint(0)  # pages opened, home+4c
    w.vint(avatar['expLevel'])  # initial snapshot already includes this level; no pending level-up
    w.reference(0)  # home+100
    for _ in range(3):
        w.vint(-1)  # home+c0,c4,c8 constructor defaults
    w.timer(IDLE_TIMER)  # shop cycle
    w.vint(0)  # shop spells
    w.timer(IDLE_TIMER)  # replay share cooldown
    w.timer(IDLE_TIMER)  # team mail cooldown
    for _ in range(6):
        w.vint(0)  # home+140,144,fc,10c,110,114
    w.vint(0)  # seen replay list
    w.string('')  # home+11c
    w.vint(0)  # spell list, home+130
    encode_avatar(w, avatar, account)
    w.vint(0)  # OwnHomeDataMessage+38
    return bytes(w.data)


def encode_avatar(w, avatar, account):
    """Native avatar body shared by the home fixture and the battle snapshot."""
    for _ in range(3):  # avatar/account/home IDs: helper uses pairs of VInts
        for value in account:
            w.vint(value)
    if not isinstance(avatar['name'], str) or not 1 <= len(avatar['name'].encode('utf-8')) <= 200:
        raise ValueError('Invalid avatar name')
    w.string(avatar['name'])
    w.boolean(avatar['nameSet'])
    w.vint(0)  # name change state
    w.reference(avatar['arena'])
    w.vint(0)  # score, avatar+34
    w.boolean(False)  # avatar+38
    w.vint(6)  # exact data-slot array count checked by the native decoder
    w.vint(len(avatar['resources']))
    for resource in avatar['resources']:
        if resource['data'] // 1000000 != 5 or resource['count'] < 0:
            raise ValueError('Invalid resource slot')
        w.reference(resource['data'])
        w.vint(resource['count'])
    for _ in range(5):
        w.vint(0)  # remaining data-slot arrays
    for field in ('diamonds', 'freeDiamonds', 'expPoints', 'expLevel'):
        if avatar[field] < 0:
            raise ValueError('Negative avatar resource/level')
        w.vint(avatar[field])
    w.vint(0)  # avatar+64
    w.boolean(False)  # no alliance
    for _ in range(6):
        w.vint(0)  # battle/win/loss/draw/NPC win/NPC loss counters
    w.boolean(False)  # avatar+b0
