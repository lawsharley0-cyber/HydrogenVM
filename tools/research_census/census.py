import struct, subprocess, re, json, sys, os, collections
ROOT="/home/claude/research_notes/Emulator compatibility strategy/work"
APPS={"AngryBirds":("ipa/AngryBirds_v1.0_os30/Payload/AngryBirds.app/AngryBirds",None),
"ArtOfGlow":("ipa/Art_Of_Glow_v1.1.2_os313/Payload/ArtOfGlowAds.app/ArtOfGlowAds","armv7"),
"TinyTower":("ipa/Tiny_Tower_v1.0_os30/Payload/Tiny Tower.app/Tiny Tower","armv7")}
def slice_bytes(path,arch):
    d=open(path,'rb').read()
    magic=struct.unpack('>I',d[:4])[0]
    if magic==0xcafebabe:
        n=struct.unpack('>I',d[4:8])[0]
        for i in range(n):
            ct,cs,off,size,al=struct.unpack('>5I',d[8+20*i:28+20*i])
            if (arch=='armv7' and cs==9) or (arch is None): return d[off:off+size]
    return d
def sections(m):
    ncmds=struct.unpack('<I',m[16:20])[0]; off=28; secs={}; segs=[]
    for _ in range(ncmds):
        cmd,sz=struct.unpack('<II',m[off:off+8])
        if cmd==1:
            segname=m[off+8:off+24].rstrip(b'\0').decode(); nsects=struct.unpack('<I',m[off+48:off+52])[0]
            so=off+56
            for _ in range(nsects):
                sn=m[so:so+16].rstrip(b'\0').decode(); sg=m[so+16:so+32].rstrip(b'\0').decode()
                addr,size,foff=struct.unpack('<III',m[so+32:so+44]); secs[(sg,sn)]=(addr,size,foff); so+=68
        off+=sz
    return secs
def cstr_at(m,secs,vm):
    for (sg,sn),(a,s,f) in secs.items():
        if a<=vm<a+s and sn in('__objc_methname','__cstring'):
            e=m.index(b'\0',f+vm-a); return m[f+vm-a:e].decode('utf8','replace')
def run(*a): return subprocess.run(list(a),capture_output=True,text=True).stdout
out={}
for app,(rel,arch) in APPS.items():
    p=os.path.join(ROOT,rel); m=slice_bytes(p,arch); secs=sections(m)
    a,s,f=secs[('__DATA','__objc_selrefs')]
    sels=collections.Counter()
    for i in range(s//4):
        v=struct.unpack('<I',m[f+4*i:f+4*i+4])[0]; n=cstr_at(m,secs,v)
        if n: sels[n]+=1
    meta=run('llvm-objdump','--macho','--objc-meta-data',*(['--arch',arch] if arch else []),p)
    defined=set(re.findall(r'\n\t\t\s+name 0x[0-9a-f]+ (\S+)\n\t\t\s+types',meta))
    nm=run('llvm-nm','-m','-u',*(['--arch',arch] if arch else []),p)
    imps=[]
    for l in nm.splitlines():
        mm=re.match(r'\s+\(undefined[^)]*\) (?:weak )?external (\S+)(?: \(from (\S+)\))?',l)
        if mm: imps.append((mm.group(1),mm.group(2) or '?', 'lazy' in l))
    out[app]=dict(imports=imps,selrefs=dict(sels),defined=sorted(defined),
      called_via_msgSend=0)
json.dump(out,open(os.path.join(ROOT,'census_apps.json'),'w'),indent=0)
for a,v in out.items():
    print(a,len(v['imports']),'imports',len(v['selrefs']),'selectors',len(v['defined']),'app-defined sels')
