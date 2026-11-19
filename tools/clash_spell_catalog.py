"""Stable spell IDs and proven field capabilities for pinned client data."""
import json
from clash_bot_policy import read_table

def catalog(bundle):
    shots={r['Name']:r for r in read_table(bundle,'projectiles') if r.get('Name')}
    areas={r['Name']:r for r in read_table(bundle,'area_effect_objects') if r.get('Name')}
    result=[]
    def positive(row,key):
        try:return int(row.get(key) or 0)>0
        except ValueError:raise ValueError('Invalid spell field: '+key)
    for index,row in enumerate(r for r in read_table(bundle,'spells_other') if r.get('Name')):
        if str(row.get('NotInUse','')).lower()=='true':continue
        area_name=row.get('AreaEffectObject') or '';area=areas.get(area_name,{})
        names=list(dict.fromkeys(n for n in [row.get('Projectile'),row.get('CustomFirstProjectile'),area.get('Projectile')] if n))
        if len(names)>3 or any(n not in shots for n in names) or (area_name and area_name not in areas):raise ValueError('Unresolved spell effect')
        damage=any(positive(shots[n],'Damage') for n in names)
        radius=positive(row,'Radius') or positive(area,'Radius') or any(positive(shots[n],'Radius') for n in names)
        # Freeze's buff time is CSV-driven separately: do not advertise life-only
        # changes as an actual freeze-duration editor. Single-tick areas stay native.
        duration=positive(area,'LifeDuration') and int(area['LifeDuration'])>1 and not positive(area,'BuffTime')
        if positive(area,'HitSpeed') and positive(area,'BuffTime'):
            duration=int(area['HitSpeed'])<=int(area['BuffTime'])<int(area.get('LifeDuration') or 0)
        speed=any(positive(shots[n],'Speed') for n in names)
        mirror=str(row.get('Mirror','')).lower()=='true'
        caps=0 if mirror else int(damage)|int(radius)<<1|int(duration)<<2|int(speed)<<3|16
        result.append(dict(id=28000000+index,name=row['Name'],shots=names,area=area_name,caps=caps))
    if not 1<=len(result)<=32:raise ValueError('Unsupported spell count')
    return result

def header(bundle):
    rows=catalog(bundle)
    out=[f'#define GUEST_SPELL_COUNT {len(rows)}','struct SpellDef {unsigned id,caps;const char *name,*shots[3];};','static const struct SpellDef spell_defs[]={']
    for r in rows:out.append('{'+str(r['id'])+','+str(r['caps'])+','+json.dumps(r['name'])+',{'+','.join(json.dumps(n) for n in r['shots']+['']*(3-len(r['shots'])))+'}},')
    return '\n'.join(out+['};'])+'\n'
