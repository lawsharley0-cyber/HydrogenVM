/* On-device SC assembly. Never modifies game saves, definitions, or old revisions. */
#include "photo_lzma.inc"
struct PhotoBytes {u8 *data;u32 size,cap;int ok;};
static u32 ph_u16(const u8 *p){return p[0]|((u32)p[1]<<8);}
static u32 ph_u32(const u8 *p){return ph_u16(p)|(ph_u16(p+2)<<16);}
static void ph_bytes(struct PhotoBytes *b,const void *p,u32 n){if(!b->ok || n>b->cap-b->size){b->ok=0;return;}memcpy(b->data+b->size,p,n);b->size+=n;}
static void ph_byte(struct PhotoBytes *b,u32 v){u8 c=(u8)v;ph_bytes(b,&c,1);}
static void ph_word(struct PhotoBytes *b,u32 v){ph_byte(b,v);ph_byte(b,v>>8);}
static void ph_dword(struct PhotoBytes *b,u32 v){ph_word(b,v);ph_word(b,v>>16);}
static void ph_tag(struct PhotoBytes *b,u32 type,u32 n){ph_byte(b,type);ph_dword(b,n);}
static struct PhotoBytes ph_buffer(u32 cap){struct PhotoBytes b={CALL(API_malloc,u8*,(u32))(cap),0,cap,1};if(!b.data)b.ok=0;return b;}
static void ph_free(struct PhotoBytes *b){if(b->data)CALL(f_free,void,(void*))(b->data);b->data=0;}
static int ph_file(const char *path,const void *data,u32 size){
    void *f=CALL(f_open,void*,(const char*,const char*))(path,"wb");if(!f)return 0;
    int ok=CALL(f_write,u32,(const void*,u32,u32,void*))(data,1,size,f)==size;
    int finished=forge_finish(f);return ok && finished;
}
static int ph_copy(const char *source,const char *dest,u32 skip){
    void *a=CALL(f_open,void*,(const char*,const char*))(source,"rb");if(!a)return 0;
    if(skip && CALL(symbol("fseek"),int,(void*,int,int))(a,(int)skip,0)){CALL(f_close,int,(void*))(a);return 0;}
    void *b=CALL(f_open,void*,(const char*,const char*))(dest,"wb");if(!b){CALL(f_close,int,(void*))(a);return 0;}
    u8 bytes[4096];u32 n,total=0;int ok=1;
    while((n=CALL(f_read,u32,(void*,u32,u32,void*))(bytes,1,sizeof bytes,a))){total+=n;if(total>16777216u || CALL(f_write,u32,(const void*,u32,u32,void*))(bytes,1,n,b)!=n){ok=0;break;}}
    ok=ok && !CALL(f_error,int,(void*))(a);int close=CALL(f_close,int,(void*))(a),finish=forge_finish(b);return ok && !close && finish;
}
static struct PhotoBytes ph_unpack(const char *path,const char *stage){
    struct PhotoBytes out={0,0,0,0};u8 header[35];
    void *f=CALL(f_open,void*,(const char*,const char*))(path,"rb");if(!f)return out;
    int ok=CALL(f_read,u32,(void*,u32,u32,void*))(header,1,35,f)==35;CALL(f_close,int,(void*))(f);
    if(!ok || header[0]!='S' || header[1]!='C' || header[5]!=1 || header[9]!=16)return out;
    u32 size=ph_u32(header+31);if(!size || size>33554432u)return out;
    char temp[600];path_append(temp,stage,"/decode.tmp",sizeof temp);if(!ph_copy(path,temp,26))return out;
    void *reader=CALL(API_malloc,void*,(u32))(0x1009c);if(!reader)return out;
    CALL(0x185b29,void,(void*,const char*,int))(reader,temp,1);
    if(!CALL(0x186041,int,(void*))(reader) && CALL(0x186035,u32,(void*))(reader)==size){
        out=ph_buffer(size);if(out.ok){CALL(0x185f01,void,(void*,int,void*))(reader,(int)size,out.data);out.size=size;
            u8 digest[16];CALL(symbol("CC_MD5"),void*,(const void*,u32,void*))(out.data,size,digest);
            for(u32 i=0;i<16;++i)if(digest[i]!=header[10+i])out.ok=0;
        }
    }
    CALL(0x185c75,void,(void*))(reader);CALL(f_free,void,(void*))(reader);
    CALL(symbol("remove"),int,(const char*))(temp);return out;
}
static struct PhotoBytes ph_compress(const struct PhotoBytes *raw){
    struct PhotoBytes out=ph_buffer(raw->size*2+90);if(!out.ok || !raw->ok){out.ok=0;return out;}
    u8 h[26]={'S','C',0,0,0,1,0,0,0,16};CALL(symbol("CC_MD5"),void*,(const void*,u32,void*))(raw->data,raw->size,h+10);ph_bytes(&out,h,26);
    u32 n=photo_lzma(raw->data,raw->size,out.data+26,out.cap-26);if(!n || n+26>16777216u)out.ok=0;else out.size=n+26;return out;
}
static void ph_shape(struct PhotoBytes *b,u32 id,u32 texture,u32 width,u32 height,int feet){
    ph_tag(b,18,66);ph_word(b,id);ph_word(b,1);ph_word(b,4);ph_tag(b,22,50);ph_byte(b,texture);ph_byte(b,4);
    int left=-(int)width*10,right=(int)width*10,top=-(int)height*(feet?20:10),bottom=feet?0:(int)height*10;
    int xy[8]={left,top,right,top,right,bottom,left,bottom};for(u32 i=0;i<8;++i)ph_dword(b,(u32)xy[i]);
    u32 uv[8]={0,0,65535,0,65535,65535,0,65535};for(u32 i=0;i<8;++i)ph_word(b,uv[i]);ph_tag(b,0,0);
}
static void ph_clip(struct PhotoBytes *b,u32 id,u32 shape){
    ph_tag(b,10,33);ph_word(b,id);ph_byte(b,30);ph_word(b,1);ph_dword(b,1);ph_word(b,0);ph_word(b,65535);ph_word(b,65535);ph_word(b,1);ph_word(b,shape);ph_byte(b,255);ph_tag(b,11,3);ph_word(b,1);ph_byte(b,255);ph_tag(b,0,0);
}
static void ph_texture(struct PhotoBytes *b,u32 height,const u8 *rgba,int pixels){ph_tag(b,1,5+(pixels?128*height*4:0));ph_byte(b,0);ph_word(b,128);ph_word(b,height);if(pixels)ph_bytes(b,rgba,128*height*4);}
static void ph_hash_name(char name[48],const u8 *rgba,u32 size){
    u8 digest[16];CALL(symbol("CC_MD5"),void*,(const void*,u32,void*))(rgba,size,digest);
    const char *prefix="ModLabHash_",*hex="0123456789abcdef";u32 at=0;while(prefix[at]){name[at]=prefix[at];++at;}
    for(u32 i=0;i<16;++i){name[at++]=hex[digest[i]>>4];name[at++]=hex[digest[i]&15];}name[at]=0;
}
static int ph_troop(struct PhotoBytes *meta,struct PhotoBytes *tex,u32 slot,const u8 *rgba){
    *meta=ph_buffer(512);*tex=ph_buffer(128*160*4+32);char name[24],hash[48];forge_identifier(name,"ModLabArt_",slot);ph_hash_name(hash,rgba,128*160*4);
    u32 counts[6]={1,1,1,0,0,0};for(u32 i=0;i<6;++i)ph_word(meta,counts[i]);for(u32 i=0;i<5;++i)ph_byte(meta,0);
    ph_word(meta,2);ph_word(meta,1);ph_word(meta,1);ph_byte(meta,forge_length(name));ph_bytes(meta,name,forge_length(name));ph_byte(meta,forge_length(hash));ph_bytes(meta,hash,forge_length(hash));
    ph_tag(meta,26,0);ph_texture(meta,160,0,0);ph_shape(meta,0,0,64,80,1);ph_clip(meta,1,0);ph_tag(meta,0,0);
    ph_texture(tex,160,rgba,1);ph_tag(tex,0,0);return meta->ok && tex->ok;
}
struct PhotoExport {u32 id;const u8 *name;u32 size;int keep;};
static int ph_icon(struct PhotoBytes *meta,struct PhotoBytes *tex,u32 slot,const u8 *rgba){
    if(!meta->ok || !tex->ok || meta->size<19 || tex->size<5)return 0;
    u32 n=ph_u16(meta->data+17),at=19+n*2;if(n>128 || at>meta->size)return 0;
    struct PhotoExport exports[128];char wanted[24],hash[48];forge_identifier(wanted,"ModLabIcon_",slot);ph_hash_name(hash,rgba,128*162*4);
    u32 old=65535;
    for(u32 i=0;i<n;++i){if(at>=meta->size)return 0;u32 len=meta->data[at++];if(len==255 || len>meta->size-at)return 0;
        exports[i]=(struct PhotoExport){ph_u16(meta->data+19+i*2),meta->data+at,len,1};
        if(len==forge_length(wanted)){u32 same=1;for(u32 k=0;k<len;++k)if(exports[i].name[k]!=(u8)wanted[k])same=0;if(same)old=exports[i].id;}
        at+=len;
    }
    u32 tags=at,max_id=0,shape=65535,texture=65535;int terminal=0;
    while(at+5<=meta->size){u32 kind=meta->data[at],size=ph_u32(meta->data+at+1);at+=5;if(size>meta->size-at)return 0;if(!kind){terminal=!size && at==meta->size;break;}
        if(kind==2 || kind==18 || kind==3 || kind==10 || kind==12 || kind==14 || kind==35 || kind==7 || kind==15 || kind==20 || kind==21 || kind==25 || kind==33 || kind==44){if(size<2)return 0;u32 id=ph_u16(meta->data+at);if(id>max_id)max_id=id;
            if(id==old && kind==10){if(size!=33)return 0;shape=ph_u16(meta->data+at+17);}
        }at+=size;
    }
    if(!terminal)return 0;
    if(old!=65535){
        at=tags;while(at+5<=meta->size){u32 kind=meta->data[at],size=ph_u32(meta->data+at+1);at+=5;if(size>meta->size-at)return 0;if(!kind)break;
            if(kind==18 && size==66 && ph_u16(meta->data+at)==shape)texture=meta->data[at+11];at+=size;}
        if(texture==65535)return 0;
        for(u32 i=0;i<n;++i)if(exports[i].id==old && exports[i].size>=11){const char *p="ModLabHash_";int match=1;for(u32 k=0;k<11;++k)if(exports[i].name[k]!=(u8)p[k])match=0;if(match)exports[i].keep=0;}
    }else{if(max_id>65532)return 0;shape=max_id+1;old=max_id+2;texture=ph_u16(meta->data+4);if(texture>=255)return 0;}
    int append=texture==ph_u16(meta->data+4);if(!append && texture>=ph_u16(meta->data+4))return 0;
    u32 keep=0;for(u32 i=0;i<n;++i)keep+=exports[i].keep;if(keep+1+append>128)return 0;
    struct PhotoBytes m=ph_buffer(meta->size+512),t=ph_buffer(tex->size+128*162*4+32);
    if(!m.ok || !t.ok){ph_free(&m);ph_free(&t);return 0;}
    ph_bytes(&m,meta->data,17);if(append){m.data[0]++;m.data[2]++;m.data[4]++;}
    ph_word(&m,keep+1+append);for(u32 i=0;i<n;++i)if(exports[i].keep)ph_word(&m,exports[i].id);if(append)ph_word(&m,old);ph_word(&m,old);
    for(u32 i=0;i<n;++i)if(exports[i].keep){ph_byte(&m,exports[i].size);ph_bytes(&m,exports[i].name,exports[i].size);}
    if(append){ph_byte(&m,forge_length(wanted));ph_bytes(&m,wanted,forge_length(wanted));}ph_byte(&m,forge_length(hash));ph_bytes(&m,hash,forge_length(hash));
    ph_bytes(&m,meta->data+tags,meta->size-tags-5);if(append){ph_texture(&m,162,0,0);ph_shape(&m,shape,texture,236,300,0);ph_clip(&m,old,shape);}ph_tag(&m,0,0);
    at=0;u32 index=0;int replaced=0;terminal=0;
    while(at+5<=tex->size){u32 kind=tex->data[at],size=ph_u32(tex->data+at+1);if(size>tex->size-at-5){t.ok=0;break;}if(!kind){terminal=!size && at+5==tex->size;break;}
        if(index++==texture){ph_texture(&t,162,rgba,1);replaced=1;}else ph_bytes(&t,tex->data+at,size+5);at+=size+5;}
    if(append){ph_texture(&t,162,rgba,1);replaced=1;}ph_tag(&t,0,0);
    int ok=m.ok && t.ok && replaced && terminal && index==ph_u16(meta->data+4);if(ok){ph_free(meta);ph_free(tex);*meta=m;*tex=t;}else{ph_free(&m);ph_free(&t);}return ok;
}
static int ph_save_asset(const char *stage,u32 kind,u32 slot,struct PhotoBytes *raw,u32 rows[34][4],u32 *count,u32 changed[34]){
    struct PhotoBytes encoded=ph_compress(raw);if(!encoded.ok){ph_free(&encoded);return 0;}
    u32 i=0;while(i<*count && (rows[i][0]!=kind || rows[i][1]!=slot))++i;if(i>=34){ph_free(&encoded);return 0;}
    char name[64],suffix[68],path[600];art_asset_name(name,kind,slot);path_append(suffix,"/",name,sizeof suffix);path_append(path,stage,suffix,sizeof path);
    int ok=ph_file(path,encoded.data,encoded.size);if(ok){if(i==*count)++*count;rows[i][0]=kind;rows[i][1]=slot;rows[i][2]=encoded.size;rows[i][3]=checksum(encoded.data,encoded.size);changed[i]=1;}
    ph_free(&encoded);return ok;
}
