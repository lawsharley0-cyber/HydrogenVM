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

# Append later content without reordering the persisted first 36 GBO1 variants.
# Spell decisions and reserve/mana rules remain the original bounded policy.
LATER_REPLACEMENTS = {
    3: [('Knight','Barbarians'),('GoblinHut','BarbarianHut'),('Arrows','Rage'),
        ('BombTower','Cannon'),('Fireball','Rocket'),('Balloon','Xbow')],
    4: [('Balloon','HogRider'),('Skeletons','MinionHorde'),('Archer','Tesla'),
        ('Tombstone','InfernoTower'),('SkeletonArmy','Freeze'),('Tombstone','HogRider')],
    5: [('Archer','Wizard'),('Arrows','Zap'),('Rage','Zap'),
        ('Goblins','Wizard'),('Knight','Wizard'),('Archer','Wizard')],
    6: [('Tombstone','Mortar'),('BarbarianHut','Elixir Collector'),('Knight','Elixir Collector'),
        ('Cannon','Mortar'),('Minions','Mortar'),('Xbow','Mortar')],
}

def arena_deck_names():
    pools={1:DECKS,2:ARENA2_DECKS}
    previous=list(ARENA2_DECKS.values())
    for arena,replacements in LATER_REPLACEMENTS.items():
        current=[]
        for cards,(old,new) in zip(previous,replacements):
            if cards.count(old)!=1:raise ValueError('Ambiguous later-arena deck substitution')
            current.append([new if card==old else card for card in cards])
        pools[arena]={f'arena{arena}_deck{i}':cards for i,cards in enumerate(current)}
        previous=current
    return pools

def read_table(bundle, name):
    with (Path(bundle)/'res/csv_logic'/f'{name}.csv').open(encoding='utf-8-sig',newline='') as f:
        rows=list(csv.reader(f))
    return [dict(zip(rows[0],row)) for row in rows[2:]]

def load_policy(bundle, include_arena2=False, include_all_arenas=False):
    arenas={r['Name']:int(r['Arena']) for r in read_table(bundle,'arenas') if r.get('Name')}
    cards={}
    for data_type,table in [(26,'spells_characters'),(27,'spells_buildings'),(28,'spells_other')]:
        for index,row in enumerate(r for r in read_table(bundle,table) if r.get('Name')):
            if row['UnlockArena'] not in arenas:raise ValueError('Unknown card unlock arena: '+row['UnlockArena'])
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
    arena2=validate(ARENA2_DECKS,2) if include_arena2 or include_all_arenas else []
    pools={1:decks,2:arena2} if arena2 else {1:decks}
    profiles=[]
    if include_all_arenas:
        pools={arena:validate(names,arena) for arena,names in arena_deck_names().items()}
        rows=[r for r in read_table(bundle,'arenas') if r.get('Name') and int(r['Arena'])>0]
        rows.sort(key=lambda r:int(r['Arena']))
        if [int(r['Arena']) for r in rows]!=list(range(1,7)):
            raise ValueError('Pinned six-arena progression required')
        npcs=[r for r in read_table(bundle,'npcs') if r.get('Name')]
        for i,row in enumerate(rows):
            arena=int(row['Arena']);lower=int(row['TrophyLimit'])
            upper=int(rows[i+1]['TrophyLimit']) if i+1<len(rows) else 100001
            eligible=[(n,npc) for n,npc in enumerate(npcs) if n>=7 and lower<=int(npc.get('Trophies') or 0)<upper]
            if not eligible:raise ValueError('No native NPC for arena '+str(arena))
            for n,npc in eligible:
                level=int(npc['ExpLevel'])
                if not 1<=level<=12:raise ValueError('Unsupported native tower level')
                for deck,d in enumerate(pools[arena]):
                    profiles.append(dict(arena=arena,mission=18000000+n,deck=deck,towerLevel=level,cards=d['cards']))
        if not 36<len(profiles)<=128:raise ValueError('Unsupported arena profile count')
    globals_={r['Name']:r.get('NumberValue') for r in read_table(bundle,'globals') if r.get('Name')}
    rates=[int(globals_[key]) for key in ['MANA_REGEN_MS','MANA_REGEN_MS_END','MANA_REGEN_MS_OVERTIME']]
    if int(globals_['MAX_MANA'])!=10 or int(globals_['START_MANA'])!=5 or any(not 0<v<=60000 for v in rates):
        raise ValueError('Unsupported native mana policy')
    return dict(decks=decks,arena2Decks=arena2,arenaDecks=pools,profiles=profiles,manaRates=rates)

def export_header(policy):
    profiles=policy['profiles']
    if not profiles:raise ValueError('Export requires all validated arena profiles')
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
    lines.append(f'#define GUEST_PROFILE_EXPORT_COUNT {len(profiles)}')
    for label,key in [('arena','arena'),('mission','mission'),('deck','deck'),('tower','towerLevel')]:
        lines.append('static const int guest_profile_'+label+'[GUEST_PROFILE_EXPORT_COUNT]={'+','.join(str(p[key]) for p in profiles)+'};')
    for label,key in [('ids','id'),('levels','level')]:
        rows=['{'+','.join(str(c[key]) for c in p['cards'])+'}' for p in profiles]
        lines.append('static const int guest_profile_'+label+'[GUEST_PROFILE_EXPORT_COUNT][8]={'+','.join(rows)+'};')
    return '\n'.join(lines)+'\n'
