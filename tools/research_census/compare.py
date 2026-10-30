import json, collections, re
ROOT="/home/claude/research_notes/Emulator compatibility strategy/work"
A=json.load(open(ROOT+'/census_apps.json')); I=json.load(open(ROOT+'/implemented.json'))
ss=set(I['strict_syms']); sl=set(I['strict_sels']); cl=set(I['classes']); lit=set(I['lit_all'])
def sym_state(s):
    if s in ss: return 'strict'
    if s in lit or s[1:] in lit: return 'literal'
    return 'missing'
def cls_state(c):
    n=c.split('$_',1)[1]
    if n in cl: return 'strict'
    if n in lit: return 'literal'
    return 'missing'
def sel_state(s):
    if s in sl: return 'strict'
    if s in lit: return 'literal'
    return 'missing'
res={}
for app,v in A.items():
    kinds=collections.defaultdict(list)
    for s,lib,lazy in v['imports']:
        if s.startswith('_OBJC_CLASS_$_'): k='class'
        elif s.startswith('_OBJC_METACLASS_$_'): continue
        elif s.startswith('_OBJC_'): k='objc_data'
        elif lazy or not re.match(r'^_(k[A-Z]|NS[A-Z]|UI[A-Z]|AV[A-Z]|CF|CG|CA|GK|AL_|AUDIO|kAudio)',s) : k='func'
        else: k='data'
        kinds[k].append((s,lib))
    res[app]=kinds
    print('==',app,{k:len(x) for k,x in kinds.items()})
    for k in ('func','class'):
        c=collections.Counter(); 
        for s,l in kinds[k]:
            st=cls_state(s) if k=='class' else sym_state(s)
            c[st]+=1
        print('  ',k,dict(c))
    dsel=set(v['defined']); used=[s for s in v['selrefs'] if s not in dsel]
    c=collections.Counter(sel_state(s) for s in used)
    print('   selectors referenced not app-defined:',len(used),dict(c))
# by library
for app,kinds in res.items():
    lib=collections.defaultdict(lambda:[0,0])
    for k in ('func','class'):
        for s,l in kinds[k]:
            st=cls_state(s) if k=='class' else sym_state(s)
            lib[l][0]+=1; lib[l][1]+= st=='missing'
    print(app,{l:tuple(x) for l,x in sorted(lib.items(),key=lambda z:-z[1][0])})
# missing lists
miss={}
for app,kinds in res.items():
    m=set()
    for s,l in kinds['func']:
        if sym_state(s)=='missing': m.add(('F',s,l))
    for s,l in kinds['class']:
        if cls_state(s)=='missing': m.add(('C',s,l))
    dsel=set(A[app]['defined'])
    for s in A[app]['selrefs']:
        if s not in dsel and sel_state(s)=='missing': m.add(('S',s,'?'))
    miss[app]=m
    print(app,'missing total',len(m),collections.Counter(x[0] for x in m))
# leave-one-out
apps=list(A)
for held in apps:
    others=[a for a in apps if a!=held]
    cnt=collections.Counter(x for a in others for x in miss[a])
    ranked=[x for x,_ in cnt.most_common()]
    ranked.sort(key=lambda x:-cnt[x])
    h=miss[held]; hit=[x for x in ranked if x in h]
    print('held',held,'|missing|',len(h),'predicted list size',len(ranked),'covers',len(hit),'(%.0f%%)'%(100*len(hit)/max(1,len(h))),
      'shared by both others:',sum(1 for x in ranked if cnt[x]==2),'of which in held:',sum(1 for x in ranked if cnt[x]==2 and x in h))
common=set.intersection(*miss.values()); print('missing in all 3',len(common))
json.dump({a:sorted(m) for a,m in miss.items()},open(ROOT+'/missing.json','w'),indent=0)
# overall api use across apps by library
libs=collections.Counter()
for app,kinds in res.items():
    for k in ('func','class','data'):
        for s,l in kinds[k]: libs[(app,l)]+=1
tot=collections.defaultdict(dict)
for (a,l),n in libs.items(): tot[l][a]=n
for l,d in sorted(tot.items(),key=lambda z:-sum(z[1].values())): print(l,d)
