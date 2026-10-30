import re, os, json
SRC="/home/claude/HydrogenVM/src"; ROOT="/home/claude/research_notes/Emulator compatibility strategy/work"
strict_syms=set(); lit_syms=set(); strict_sels=set(); lit_sels=set(); classes=set(); lit_all=set()
for d,_,fs in os.walk(SRC):
    if 'thirdparty' in d: continue
    for f in fs:
        if not f.endswith(('.cpp','.hpp','.inc')): continue
        t=open(os.path.join(d,f),errors='replace').read()
        strict_syms|=set(re.findall(r'(?:\bdef<[^>]*>|defineFunction|defineData|defineDataBlock|defAlias|alias)\(\s*"(_[^"]+)"',t))
        strict_sels|=set(re.findall(r'\.(?:m|cm|method|classMethod|alias)(?:<[^>]*>)?\(\s*"([^"]+)"',t))
        classes|=set(re.findall(r'(?:defineClass|stubClass|declareClass)\(\s*"([A-Za-z0-9_$]+)"',t))
        for s in re.findall(r'"([^"\\\n]{1,120})"',t):
            lit_all.add(s)
json.dump(dict(strict_syms=sorted(strict_syms),strict_sels=sorted(strict_sels),classes=sorted(classes),lit_all=sorted(lit_all)),open(ROOT+'/implemented.json','w'))
print(len(strict_syms),len(strict_sels),len(classes),len(lit_all))
