"""Pinned card mechanics and safe replacement references, generated privately."""
import json
from clash_bot_policy import read_table

def catalog(bundle):
    troops=[r for r in read_table(bundle,'characters') if r.get('Name')]
    shots=[r for r in read_table(bundle,'projectiles') if r.get('Name')]
    by_shot={r['Name']:r for r in shots}
    cards=[]
    for group,table in [(26,'spells_characters'),(27,'spells_buildings'),(28,'spells_other')]:
        for index,row in enumerate(r for r in read_table(bundle,table) if r.get('Name')):
            projectile=by_shot.get(row.get('Projectile'),{})
            if row.get('SummonCharacter') or projectile.get('SpawnCharacter'):
                cards.append(dict(id=group*1000000+index,name=row['Name']))
    # Avoid recursive factories: replacement troops cannot themselves summon on
    # a timer, on death, or through a projectile. Originals remain available as Default.
    safe_troops=[r['Name'] for r in troops if not r.get('SpawnCharacter') and not r.get('DeathSpawnCharacter') and not by_shot.get(r.get('Projectile'),{}).get('SpawnCharacter')]
    safe_shots=[r['Name'] for r in shots if not r.get('SpawnCharacter') and not r.get('SpawnAreaEffectObject')]
    if not cards or not safe_troops or not safe_shots:raise ValueError('Empty mechanics catalog')
    if len(cards)>64 or len(safe_troops)>64 or len(safe_shots)>64:raise ValueError('Mechanics catalog exceeds capacity')
    return cards,safe_troops,safe_shots

def header(bundle):
    cards,troops,shots=catalog(bundle)
    lines=[f'#define GUEST_MECH_CARD_COUNT {len(cards)}',f'#define GUEST_MECH_TROOP_COUNT {len(troops)}',f'#define GUEST_MECH_SHOT_COUNT {len(shots)}',
           'struct MechCardDef {unsigned id;const char *name;};','static const struct MechCardDef mech_cards[]={']
    lines += ['{'+str(r['id'])+','+json.dumps(r['name'])+'},' for r in cards]
    lines += ['};','static const char *mech_troop_names[]={'+','.join(map(json.dumps,troops))+'};','static const char *mech_shot_names[]={'+','.join(map(json.dumps,shots))+'};']
    return '\n'.join(lines)+'\n'
