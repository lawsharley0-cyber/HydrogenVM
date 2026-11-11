/* Optional static image packs are tied to the immutable definition prefix. */
static u32 art_ready,art_valid,art_rejected,art_flags[FORGE_MAX_CARDS],art_file_count;
static u32 art_files[34][4];
static char art_folder[512];
static void art_generation(char out[16],u32 generation){
    const char *hex="0123456789abcdef";out[0]='/';out[1]='g';for(u32 i=0;i<8;++i)out[2+i]=hex[(generation>>(28-i*4))&15];out[10]=0;
}
static void art_asset_name(char out[64],u32 kind,u32 slot){
    if(kind<2){path_append(out,"",kind?"ui_spells_tex.sc":"ui_spells.sc",64);return;}
    char id[24];forge_identifier(id,"ModLabArt_",slot);path_append(out,id,kind==2?".sc":"_tex.sc",64);
}
static int art_check_file(const u32 r[4]){
    char name[64],suffix[68],path[600];art_asset_name(name,r[0],r[1]);path_append(suffix,"/",name,sizeof suffix);path_append(path,art_folder,suffix,sizeof path);
    void *f=CALL(f_open,void*,(const char*,const char*))(path,"rb");if(!f)return 0;
    u32 remain=r[2],hash=2166136261u;u8 data[4096];int ok=1;
    if(!remain || remain>16777216u)ok=0;
    while(ok && remain){u32 n=remain>sizeof data?sizeof data:remain;
        if(CALL(f_read,u32,(void*,u32,u32,void*))(data,1,n,f)!=n){ok=0;break;}
        for(u32 b=0;b<n;++b)hash=(hash^data[b])*16777619u;remain-=n;
    }
    u8 extra;ok=ok && !CALL(f_read,u32,(void*,u32,u32,void*))(&extra,1,1,f) && !CALL(f_error,int,(void*))(f);
    int closed=CALL(f_close,int,(void*))(f);return ok && !closed && hash==r[3];
}
static void art_initialize(void){
    if(art_ready)return;art_ready=1;
    const char *home=CALL(symbol("getenv"),const char*,(const char*))("HOME");
    path_append(art_folder,home,"/Documents/ClashForgeArt-v1",sizeof art_folder);
    char path[560];path_append(path,art_folder,"/manifest.bin",sizeof path);
    void *f=CALL(f_open,void*,(const char*,const char*))(path,"rb");if(!f)return;
    u32 h[8],body[153];int ok=CALL(f_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h;
    ok=ok && ((h[0]==0x31545241u && h[1]==1) || (h[0]==0x32545241u && h[1]==2)) && h[2]>0 && h[2]<=forge_loaded_count && h[2]<=FORGE_MAX_CARDS && h[3]==checksum((const u8*)forge_records,h[2]*sizeof *forge_records) && h[4]==GUEST_ART_SOURCE_TOKEN && h[5]>0 && h[5]<=34 && h[7]==checksum((const u8*)h,28);
    u32 words=ok?h[2]+h[5]*4+(h[1]==2):0;ok=ok && words<=153 && CALL(f_read,u32,(void*,u32,u32,void*))(body,4,words,f)==words;
    u8 extra;ok=ok && !CALL(f_read,u32,(void*,u32,u32,void*))(&extra,1,1,f) && !CALL(f_error,int,(void*))(f);
    int closed=CALL(f_close,int,(void*))(f);ok=ok && !closed && h[6]==checksum((const u8*)body,words*4);
    u32 any_icon=0,ui_seen=0,seen[FORGE_MAX_CARDS]={0};u32 *values=body;
    if(ok && h[1]==2){if(!body[0])ok=0;else{char gen[16],root[512];memcpy(root,art_folder,sizeof root);art_generation(gen,body[0]);path_append(art_folder,root,gen,sizeof art_folder);++values;}}

    if(ok){
        for(u32 slot=0;slot<h[2];++slot){art_flags[slot]=values[slot];if(art_flags[slot]>3)ok=0;
#ifdef GUEST_FORGE_SPELLS
            if(forge_records[slot].base>=28000000u && (art_flags[slot]&2))ok=0;
#endif
            any_icon|=art_flags[slot]&1;}
        for(u32 i=0;ok && i<h[5];++i){u32 *row=values+h[2]+i*4,kind=row[0],slot=row[1];
            if(kind>3 || slot>=h[2]){ok=0;break;}
            if(kind<2){u32 bit=1u<<kind;if(slot || !any_icon || ui_seen&bit){ok=0;break;}ui_seen|=bit;}
            else {u32 bit=1u<<(kind-2);if(!(art_flags[slot]&2) || seen[slot]&bit){ok=0;break;}seen[slot]|=bit;}
            if(!art_check_file(row)){ok=0;break;}
            for(u32 k=0;k<4;++k)art_files[i][k]=row[k];
        }
        if((any_icon && ui_seen!=3) || (!any_icon && ui_seen))ok=0;
        for(u32 slot=0;slot<h[2];++slot)if(((art_flags[slot]&2)!=0)!=(seen[slot]==3))ok=0;
    }
    if(!ok){art_rejected=1;memset(art_flags,0,sizeof art_flags);CALL(API_puts,int,(const char*))("Card Forge: optional art pack rejected; files retained, original artwork used");return;}
    art_valid=1;art_file_count=h[5];CALL(API_printf,int,(const char*,u32))("Card Forge: validated custom image pack for %u definitions\n",h[2]);
}
static void *art_redirect(const char *path,const char *mode,int *handled){
    if(!art_valid)return 0;
    const char *leaf=path;for(const char *p=path;*p;++p)if(*p=='/')leaf=p+1;
    // Only bundled res/sc assets. Native swfcache/sc files are already
    // decompressed; substituting a compressed source there corrupts cache reads.
    u32 prefix=(u32)(leaf-path);const char *suffix="res/sc/";
    if(prefix<7 || (prefix>7 && path[prefix-8]!='/'))return 0;
    for(u32 k=0;k<7;++k)if(path[prefix-7+k]!=suffix[k])return 0;
    for(u32 i=0;i<art_file_count;++i){char name[64];art_asset_name(name,art_files[i][0],art_files[i][1]);if(!forge_equal(leaf,name))continue;
        char suffix[68],dest[600];path_append(suffix,"/",name,sizeof suffix);path_append(dest,art_folder,suffix,sizeof dest);*handled=1;
        return CALL(f_open,void*,(const char*,const char*))(dest,mode);
    }
    return 0;
}
