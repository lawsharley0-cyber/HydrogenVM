"""Pack user PNGs into isolated legacy SC assets for an existing FGR1 profile.

Original format research: jeanbmar/sc-tools (SC1 tagged geometry/timelines)
and Galaxy1036/scPacker (texture/pixel/compression framing). No vendor assets
or third-party implementation is bundled. The first import supports static
single-image troop appearances; it does not synthesize animation frames.
"""
import argparse,hashlib,json,lzma,struct
from pathlib import Path
from clash_custom_cards import decode_registry,fnv

MAGIC=0x31545241 # ART1
MAX_RAW=32*1024*1024

def unpack_sc(data):
    if data[:2]==b'SC':
        if len(data)<26 or struct.unpack_from('>II',data,2)!=(1,16):raise ValueError('Unsupported SC wrapper')
        digest=data[10:26];data=data[26:]
    else:digest=None
    if len(data)<9:raise ValueError('Truncated SC compression header')
    size=struct.unpack_from('<I',data,5)[0]
    if size>MAX_RAW:raise ValueError('SC asset exceeds size bound')
    decoder=lzma.LZMADecompressor(format=lzma.FORMAT_ALONE,memlimit=64*1024*1024)
    raw=decoder.decompress(data[:9]+bytes(4)+data[9:],max_length=size+1)
    if len(raw)!=size or not decoder.eof or decoder.unused_data:raise ValueError('Invalid SC compressed extent')
    if digest and hashlib.md5(raw).digest()!=digest:raise ValueError('SC digest mismatch')
    return raw

def pack_sc(raw):
    if len(raw)>MAX_RAW:raise ValueError('SC asset exceeds size bound')
    encoded=lzma.compress(raw,format=lzma.FORMAT_ALONE,filters=[dict(id=lzma.FILTER_LZMA1,dict_size=262144,lc=3,lp=0,pb=2)])
    return b'SC'+struct.pack('>II',1,16)+hashlib.md5(raw).digest()+encoded[:5]+struct.pack('<I',len(raw))+encoded[13:]

def tag(kind,payload=b''):return struct.pack('<BI',kind,len(payload))+payload

def parse_metadata(raw):
    if len(raw)<19:raise ValueError('Truncated SC metadata')
    counts=list(struct.unpack_from('<6H',raw));at=17
    n=struct.unpack_from('<H',raw,at)[0];at+=2
    if n>8192 or at+2*n>len(raw):raise ValueError('Invalid SC exports')
    ids=struct.unpack_from('<'+'H'*n,raw,at);at+=2*n;exports=[]
    for export in ids:
        if at>=len(raw):raise ValueError('Truncated SC export name')
        length=raw[at];at+=1
        if length==255 or at+length>len(raw):raise ValueError('Invalid SC export name')
        name=raw[at:at+length];at+=length;exports.append((export,name))
    tags=[]
    while at<len(raw):
        if at+5>len(raw):raise ValueError('Truncated SC tag')
        kind,size=struct.unpack_from('<BI',raw,at);at+=5
        if at+size>len(raw):raise ValueError('SC tag exceeds asset')
        payload=raw[at:at+size];at+=size
        if kind==0:
            if size or at!=len(raw):raise ValueError('Invalid SC terminal tag')
            break
        tags.append((kind,payload))
    else:raise ValueError('Missing SC terminal tag')
    return counts,raw[12:17],exports,tags

def encode_metadata(counts,padding,exports,tags):
    if any(not 0<=v<65536 for v in counts) or len(exports)>65535:raise ValueError('SC count exceeds representation')
    names=b''.join(bytes([len(name)])+name for _,name in exports)
    if any(len(name)>254 for _,name in exports):raise ValueError('SC export name exceeds bound')
    return struct.pack('<6H',*counts)+padding+struct.pack('<H',len(exports))+b''.join(struct.pack('<H',i) for i,_ in exports)+names+b''.join(tag(k,p) for k,p in tags)+tag(0)

def bitmap_shape(shape_id,texture_index,width,height,feet=False):
    left=-width*10;right=width*10;top=-height*20 if feet else -height*10;bottom=0 if feet else height*10
    xy=[left,top,right,top,right,bottom,left,bottom]
    uv=[0,0,65535,0,65535,65535,0,65535]
    bitmap=struct.pack('<BB8i8H',texture_index,4,*xy,*uv)
    return struct.pack('<HHH',shape_id,1,4)+tag(22,bitmap)+tag(0)

def static_clip(clip_id,shape_id):
    # One visible child; 0xffff means the original identity transform/color.
    return struct.pack('<HBHI3HHH',clip_id,30,1,1,0,65535,65535,1,shape_id)+b'\xff'+tag(11,struct.pack('<HB',1,255))+tag(0)

def pixels(path,width,height):
    from PIL import Image,ImageOps
    with Image.open(path) as image:
        if image.format!='PNG' or image.width>4096 or image.height>4096:raise ValueError('Use a PNG no larger than 4096 pixels per side')
        image.load();image=ImageOps.contain(image.convert('RGBA'),(width,height),Image.Resampling.LANCZOS)
        canvas=Image.new('RGBA',(width,height));canvas.alpha_composite(image,((width-image.width)//2,height-image.height))
        return canvas.convert('RGBa').tobytes()

def texture(width,height,rgba):
    if len(rgba)!=width*height*4:raise ValueError('Invalid RGBA extent')
    return struct.pack('<BHH',0,width,height)+rgba

def append_icon_pair(meta,tex,items):
    counts,padding,exports,tags=parse_metadata(meta)
    known={2,18,3,10,12,14,35,7,15,20,21,25,33,44}
    next_id=max([i for i,_ in exports]+[struct.unpack_from('<H',p)[0] for k,p in tags if k in known]+[-1])+1
    if not tex.endswith(tag(0)):raise ValueError('Texture stream lacks terminator')
    tex=tex[:-5]
    for slot,rgba in items:
        shape_id,clip_id=next_id,next_id+1;next_id+=2
        if next_id>=65535 or counts[2]>=255:raise ValueError('SC identity capacity exceeded')
        name=f'ModLabIcon_{slot:04d}'.encode();exports.append((clip_id,name))
        # Bind the metadata cache key to pixels even if their dimensions stay fixed.
        exports.append((clip_id,b'ModLabHash_'+hashlib.sha256(rgba).hexdigest()[:24].encode()))
        tags += [(1,struct.pack('<BHH',0,128,162)),(18,bitmap_shape(shape_id,counts[2],236,300)),(10,static_clip(clip_id,shape_id))]
        tex+=tag(1,texture(128,162,rgba));counts[0]+=1;counts[1]+=1;counts[2]+=1
    return encode_metadata(counts,padding,exports,tags),tex+tag(0)

def troop_pair(slot,rgba):
    clip=static_clip(1,0);name=f'ModLabArt_{slot:04d}'.encode()
    tags=[(26,b''),(1,struct.pack('<BHH',0,128,160)),(18,bitmap_shape(0,0,64,80,feet=True)),(10,clip)]
    exports=[(1,name),(1,b'ModLabHash_'+hashlib.sha256(rgba).hexdigest()[:24].encode())]
    return encode_metadata([1,1,1,0,0,0],bytes(5),exports,tags),tag(1,texture(128,160,rgba))+tag(0)

def source_token(bundle):
    return fnv(struct.pack('<II',*[fnv((Path(bundle)/'res/sc'/n).read_bytes()) for n in ['ui_spells.sc','ui_spells_tex.sc']]))

def make_pack(bundle,registry,images):
    cards=decode_registry(registry)['cards'];flags=[0]*len(cards);assets={};icons=[]
    if not isinstance(images,dict) or set(images)-set(map(str,range(len(cards)))):raise ValueError('Image slots must name existing custom definitions')
    for key,images_for_card in images.items():
        slot=int(key)
        if not isinstance(images_for_card,dict) or not images_for_card or set(images_for_card)-{'icon','troop'}:raise ValueError('Image entries accept icon and/or troop PNG paths')
        if cards[slot]['baseCardId'] >= 28000000 and 'troop' in images_for_card:
            raise ValueError('Spell clones accept icon artwork only')
        if 'icon' in images_for_card:flags[slot]|=1;icons.append((slot,pixels(images_for_card['icon'],128,162)))
        if 'troop' in images_for_card:
            flags[slot]|=2;meta,tex=troop_pair(slot,pixels(images_for_card['troop'],128,160));assets[(2,slot)]=pack_sc(meta);assets[(3,slot)]=pack_sc(tex)
    if not any(flags):raise ValueError('Art pack needs at least one image')
    if icons:
        meta,tex=append_icon_pair(unpack_sc((Path(bundle)/'res/sc/ui_spells.sc').read_bytes()),unpack_sc((Path(bundle)/'res/sc/ui_spells_tex.sc').read_bytes()),icons)
        assets[(0,0)]=pack_sc(meta);assets[(1,0)]=pack_sc(tex)
    body=struct.pack('<'+'I'*len(flags),*flags)+b''.join(struct.pack('<4I',kind,slot,len(data),fnv(data)) for (kind,slot),data in sorted(assets.items()))
    h=struct.pack('<7I',MAGIC,1,len(cards),fnv(registry[20:]),source_token(bundle),len(assets),fnv(body))
    return h+struct.pack('<I',fnv(h))+body,assets

def asset_name(kind,slot):
    return ['ui_spells.sc','ui_spells_tex.sc',f'ModLabArt_{slot:04d}.sc',f'ModLabArt_{slot:04d}_tex.sc'][kind]

def main():
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--bundle',type=Path,required=True);p.add_argument('--registry',type=Path,required=True);p.add_argument('--images',type=Path,required=True);p.add_argument('--out',type=Path,required=True);a=p.parse_args()
    try:
        images=json.loads(a.images.read_text(encoding='utf-8'))
        if not isinstance(images,dict):raise ValueError('Images JSON must be a slot-to-images object')
        for row in images.values():
            if not isinstance(row,dict):raise ValueError('Each slot needs an icon/troop image object')
            for k,path in row.items():
                if not isinstance(path,str) or not path:raise ValueError('PNG paths must be nonempty strings')
                row[k]=str((a.images.parent/path).resolve())
        manifest,assets=make_pack(a.bundle,a.registry.read_bytes(),images)
    except (ValueError,OSError,TypeError) as error:p.error(str(error))
    a.out.mkdir(parents=True,exist_ok=False)
    for (kind,slot),data in assets.items():(a.out/asset_name(kind,slot)).write_bytes(data)
    (a.out/'manifest.bin').write_bytes(manifest)
    print(json.dumps(dict(output=str(a.out),files=len(assets),slots=len(images),troopAppearance='static image; no synthesized animations'),indent=2))

if __name__=='__main__':main()
