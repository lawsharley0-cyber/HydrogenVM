"""Bounded decoding of pinned client turns for local NPC experiments.

Only native spell and battle-tutorial commands are understood. This records
what the native client committed; it does not confirm or replay commands.

Ported from the October 21, 2026 handoff into ``tools/clash/``. ``tools/`` is
added to ``sys.path`` so ``clash_wire`` resolves. This file contains no client
bytes, tokens, or save data.
"""
import sys
from pathlib import Path

_TOOLS = Path(__file__).resolve().parents[1]
if str(_TOOLS) not in sys.path:
    sys.path.insert(0, str(_TOOLS))

from clash_wire import Reader


def decode_turn(payload, account):
    r = Reader(payload)
    tick = r.vint()
    checksum, count = r.vint(), r.vint()
    if tick < 0 or not 0 <= count <= 64:
        raise ValueError('Invalid client turn tick/count')
    commands = []

    def base():
        given, sequence, high, low = (r.vint() for _ in range(4))
        if (high, low) not in (tuple(account), (-1, 0)):
            raise ValueError('Turn executor does not belong to this local battle')
        return {'givenTick': given, 'sequence': sequence, 'account': [high, low]}

    for _ in range(count):
        kind = r.vint()
        if kind == 2:
            tutorial = r.reference()
            if tutorial // 1000000 != 48:
                raise ValueError('Invalid battle tutorial reference')
            command = {'type': kind, 'tutorial': tutorial, **base()}
        elif kind == 1:
            command = {'type': kind, **base(), 'spellIndex': r.vint()}
            if r.boolean():
                raise ValueError('Custom spell payload is not supported by the turn metadata decoder')
            command['x'], command['y'] = r.vint(), r.vint()
            if not 0 <= command['x'] < 18000 or not 0 <= command['y'] < 32000:
                raise ValueError('Spell target exceeds the initial arena bounds')
        else:
            raise ValueError(f'Unsupported local battle command {kind}')
        commands.append(command)
    if r.integer() != -1 or r.offset != len(payload):
        raise ValueError('Unsupported debug trailer or trailing turn bytes')
    return {'tick': tick, 'checksum': checksum, 'commands': commands}
