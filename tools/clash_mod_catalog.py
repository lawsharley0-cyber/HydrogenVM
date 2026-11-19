"""Generate private, pinned unit metadata for the guest editor; no assets in Git."""
from clash_bot_policy import read_table

def catalog(bundle):
    chars={r['Name']:r for r in read_table(bundle,'characters') if r.get('Name')}
    chars.update({r['Name']:r for r in read_table(bundle,'buildings') if r.get('Name')})
    entries=[];seen=set()
    for data_type,table in [(26,'spells_characters'),(27,'spells_buildings')]:
        for index,row in enumerate(r for r in read_table(bundle,table) if r.get('Name')):
            name=row.get('SummonCharacter')
            if not row.get('Name') or not name or name in seen:continue
            if name not in chars:raise ValueError('Mod unit missing native character: '+name)
            char=chars[name];seen.add(name);entries.append(dict(name=name,projectile=char.get('Projectile') or '',cardId=data_type*1000000+index))
    if not 1<=len(entries)<=64:raise ValueError('Unsupported mod catalog size')
    return entries

def header(entries):
    import json
    lines=[f'#define GUEST_MOD_UNIT_COUNT {len(entries)}',
           'struct ModUnitDef {const char *name,*projectile;unsigned cardId;};',
           'static const struct ModUnitDef mod_unit_defs[GUEST_MOD_UNIT_COUNT]={']
    for e in entries:lines.append('{'+json.dumps(e['name'])+','+json.dumps(e['projectile'])+','+str(e['cardId'])+'},')
    return '\n'.join(lines+['};'])+'\n'
