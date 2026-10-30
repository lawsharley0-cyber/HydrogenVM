import json,collections,re
A=json.load(open('census_apps.json'))
T={}
for l in open('oracle_out.txt'):
    k,n,v=l.split(); T[(k,n)]=int(v)
miss={}
for app,v in A.items():
    d=set(v['defined']); m=[]
    for s,lib,lazy in v['imports']:
        if s.startswith('_OBJC_CLASS_$_'):
            n=s.split('$_',1)[1]; st=T[('C',n)]
            if st!=1: m.append(('C',n,lib,st))
        elif s.startswith('_OBJC_'): pass
        else:
            if not T[('S',s)]: m.append(('S',s,lib,0))
    for s in v['selrefs']:
        if s in d: continue
        if not T[('M',s)]: m.append(('M',s,'?',0))
    miss[app]=m
    nf=sum(1 for s,lib,lz in v['imports'] if not s.startswith('_OBJC_'))
    ncl=sum(1 for s,lib,lz in v['imports'] if s.startswith('_OBJC_CLASS_$_'))
    nsel=sum(1 for s in v['selrefs'] if s not in d)
    print(app,'symbols',nf,'missing',sum(1 for x in m if x[0]=='S'),'| classes',ncl,'missing/stub',sum(1 for x in m if x[0]=='C'),'| non-app selectors',nsel,'unimplemented-anywhere',sum(1 for x in m if x[0]=='M'))
    for k in 'SC': print('  ',k,[ (x[1],x[2]) for x in m if x[0]==k])
    ms=[x[1] for x in m if x[0]=='M']; print('   M sample',ms[:60])
    # selectors by library? no. 
json.dump(miss,open('truth_missing.json','w'))
apps=list(A)
for held in apps:
    others=[a for a in apps if a!=held]
    cnt=collections.Counter(x[:2] for a in others for x in miss[a])
    h=set(x[:2] for x in miss[held]); pred=list(cnt)
    byk=lambda S,k:{x for x in S if x[0]==k}
    print('held',held,'miss',len(h),'predicted',len(pred),'hit',len(h&set(pred)),'| funcs+classes only: miss',len(h-byk(h,'M')),'hit',len((h&set(pred))-byk(h,'M')))
