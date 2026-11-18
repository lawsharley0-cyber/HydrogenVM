"""Build private static-art CSV variants; preserve normal templates when absent."""
import csv,io,json
from pathlib import Path
from clash_custom_cards import FORGE_TABLES,generate_overlays
from clash_custom_art import source_token

def header(bundle,entries):
    originals={p:(Path(bundle)/p).read_bytes() for p in FORGE_TABLES}
    columns={p:next(csv.reader(io.StringIO(originals[p].decode('utf-8-sig')))) for p in FORGE_TABLES[:4]}
    tokens={'ModLabCard_0000':'@CARD@','ModLabUnit_0000':'@UNIT@','ModLabShot_0000':'@SHOT@','TID_MODLAB_0000':'@TID@','FORGE_NAME_TOKEN':'@NAME@'}
    lines=[f'#define GUEST_ART_SOURCE_TOKEN {source_token(bundle)}u','static const char *forge_art_templates[GUEST_MOD_UNIT_COUNT][4][4]={']
    for e in entries:
        overlays,_=generate_overlays(bundle,dict(version=1,cards=[dict(slot=0,baseCardId=e['cardId'],name='FORGE_NAME_TOKEN')]))
        variants=[]
        for flags in range(4):
            rows=[]
            for i,path in enumerate(FORGE_TABLES[:4]):
                suffix=overlays.get(path,originals[path])[len(originals[path]):].decode('utf-8')
                parsed=list(csv.reader(io.StringIO(suffix))) if suffix else []
                for row in parsed:
                    if not row:continue
                    def set_value(key,value):
                        if key not in columns[path]:return
                        at=columns[path].index(key);row.extend(['']*max(0,at+1-len(row)));row[at]=value
                    if i<2 and flags&2 and row[0]:
                        set_value('FileName','sc/@ART@.sc');set_value('BlueExportName','@ART@');set_value('RedExportName','@ART@')
                        set_value('BlueTopExportName','');set_value('RedTopExportName','');set_value('UseAnimator','false')
                    if i>=2 and flags&1 and row[0]:set_value('IconFile','@ICON@')
                out=io.StringIO(newline='');csv.writer(out,lineterminator='\r\n').writerows(parsed);text=out.getvalue()
                for a,b in tokens.items():text=text.replace(a,b)
                rows.append(json.dumps(text))
            variants.append('{'+','.join(rows)+'}')
        lines.append('{'+','.join(variants)+'},')
    return '\n'.join(lines+['};'])+'\n'
