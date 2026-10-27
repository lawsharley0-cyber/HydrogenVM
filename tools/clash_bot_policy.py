"""Authored early-arena decks; validate IDs/unlocks against the supplied pinned IPA."""
import csv
from pathlib import Path

DECKS = {
    'giant_support': ['Giant','Knight','Archer','Musketeer','Bomber','Goblins','Fireball','Arrows'],
    'prince_pressure': ['Prince','Knight','Archer','MiniPekka','SpearGoblins','Bomber','Arrows','Fireball'],
    'swarm_counter': ['SkeletonArmy','Goblins','SpearGoblins','Knight','Musketeer','Giant','Arrows','Fireball'],
    'hut_control': ['GoblinHut','Knight','Archer','SpearGoblins','Giant','MiniPekka','Arrows','Fireball'],
    'witch_push': ['Giant','Witch','Knight','Archer','Goblins','Bomber','Fireball','Arrows'],
    'barrel_bait': ['GoblinBarrel','Goblins','SpearGoblins','Knight','Valkyrie','Archer','Arrows','Fireball'],
}
ARENA2_DECKS = {
    'balloon_minions': ['Balloon','Minions','Knight','Archer','Tombstone','Goblins','Arrows','Fireball'],
    'giant_air': ['Giant','Minions','Musketeer','Skeletons','Bomber','GoblinHut','Arrows','Fireball'],
    'witch_skeletons': ['Giant','Witch','Skeletons','Minions','Knight','Archer','Arrows','Fireball'],
    'tombstone_control': ['Tombstone','BombTower','Valkyrie','Musketeer','Minions','Goblins','Arrows','Fireball'],
    'minion_counter': ['SkeletonArmy','Minions','SpearGoblins','Knight','Giant','Skeletons','Arrows','Fireball'],
    'balloon_bait': ['Balloon','GoblinBarrel','Goblins','Minions','Tombstone','Archer','Arrows','Fireball'],
}

def read_table(bundle, name):
    with (Path(bundle)/'res/csv_logic'/f'{name}.csv').open(encoding='utf-8-sig',newline='') as f:
        rows=list(csv.reader(f))
    return [dict(zip(rows[0],row)) for row in rows[2:]]

def load_policy(bundle, include_arena2=False):
    arenas={r['Name']:int(r['Arena']) for r in read_table(bundle,'arenas') if r.get('Name')}
    cards={}
    for data_type,table in [(26,'spells_characters'),(27,'spells_buildings'),(28,'spells_other')]:
        for index,row in enumerate(r for r in read_table(bundle,table) if r.get('Name')):
            cards[row['Name']]={'id':data_type*1000000+index,'arena':arenas[row['UnlockArena']],
                                'level':1 if row['Rarity']=='Common' else 0,'cost':int(row['ManaCost'])}
    def validate(source,arena):
        decks=[]
        for name,names in source.items():
            if len(names)!=8 or len(set(names))!=8:raise ValueError('Bot deck must contain eight unique cards')
            selected=[cards[n] for n in names]
            if any(c['arena']>arena for c in selected):raise ValueError(f'Bot deck exceeds Arena-{arena} unlocks: '+name)
            if any(not 0<c['cost']<=6 for c in selected):raise ValueError('Bot deck cannot afford cards with the patient reserve: '+name)
            decks.append(dict(name=name,cards=selected))
        return decks
    decks=validate(DECKS,1)
    arena2=validate(ARENA2_DECKS,2) if include_arena2 else []
    globals_={r['Name']:r.get('NumberValue') for r in read_table(bundle,'globals') if r.get('Name')}
    rates=[int(globals_[key]) for key in ['MANA_REGEN_MS','MANA_REGEN_MS_END','MANA_REGEN_MS_OVERTIME']]
    if int(globals_['MAX_MANA'])!=10 or int(globals_['START_MANA'])!=5 or any(not 0<v<=60000 for v in rates):
        raise ValueError('Unsupported native mana policy')
    return dict(decks=decks,arena2Decks=arena2,manaRates=rates)

def export_header(policy):
    lines=['#define GUEST_BOT_POLICY_GENERATED 1',f'#define GUEST_BOT_DECK_COUNT {len(policy["decks"])}']
    for label,key in [('ids','id'),('levels','level')]:
        rows=['{'+','.join(str(c[key]) for c in d['cards'])+'}' for d in policy['decks']]
        lines.append(f'static const int guest_bot_{label}[GUEST_BOT_DECK_COUNT][8]={{'+','.join(rows)+'};')
    lines.append('static const int guest_bot_mana_rates[3]={'+','.join(map(str,policy['manaRates']))+'};')
    lines.append(f'#define GUEST_ARENA2_DECK_COUNT {len(policy["arena2Decks"])}')
    if policy['arena2Decks']:
        if len(policy['arena2Decks'])!=len(policy['decks']):raise ValueError('Arena deck pool sizes differ')
        for label,key in [('ids','id'),('levels','level')]:
            rows=['{'+','.join(str(c[key]) for c in d['cards'])+'}' for d in policy['arena2Decks']]
            lines.append(f'static const int guest_arena2_{label}[GUEST_ARENA2_DECK_COUNT][8]={{'+','.join(rows)+'};')
    return '\n'.join(lines)+'\n'
