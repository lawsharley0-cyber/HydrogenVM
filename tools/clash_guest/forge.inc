/* Guest-owned definitions and regenerated CSV cache. Included after save helpers.
 * Raw dlsym file functions bypass our import hook; original assets stay read-only. */
#include "forge_format.inc"
static struct ForgeRecord forge_records[FORGE_MAX_CARDS];
static u32 forge_count,forge_loaded_count,forge_ready,forge_generated,forge_ids[FORGE_MAX_CARDS];
static u32 f_open,f_read,f_write,f_close,f_error,f_errno,f_flush,f_fileno,f_sync,f_rename,f_free;
static char forge_path[512],forge_cache[512];
static char forge_render[65536];
#ifdef GUEST_FORGE_SPELLS
static u32 forge_unit_slots[FORGE_MAX_CARDS],forge_unit_count;
static u32 forge_spell_slots[FORGE_MAX_CARDS],forge_spell_count;
#endif
#ifdef GUEST_ART_SOURCE_TOKEN
#include "forge_art.inc"
#endif
static const struct ForgeBlueprint *forge_blueprint(u32 id) {
    for(u32 i=0;i<sizeof forge_blueprints/sizeof *forge_blueprints;++i)if(forge_blueprints[i].id==id)return &forge_blueprints[i];
    return 0;
}
static void forge_initialize(void) {
    if(forge_ready)return;
    f_open=symbol("fopen");f_read=symbol("fread");f_write=symbol("fwrite");f_close=symbol("fclose");
    f_error=symbol("ferror");f_errno=symbol("__error");f_flush=symbol("fflush");f_fileno=symbol("fileno");
    f_sync=symbol("fsync");f_rename=symbol("rename");f_free=symbol("free");
    const char *home=CALL(symbol("getenv"),const char*,(const char*))("HOME");
    if(!home || !*home)fail("Card Forge: guest home missing; saves retained");
    path_append(forge_path,home,"/Documents/ClashForge-v1.bin",sizeof forge_path);
    path_append(forge_cache,home,"/Documents/ClashForge-v1",sizeof forge_cache);
    void *file=CALL(f_open,void*,(const char*,const char*))(forge_path,"rb");
    if(file) {
        u32 h[5];int ok=CALL(f_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
        ok=ok && forge_header_valid(h);u32 size=ok?h[2]*sizeof *forge_records:0;
        ok=ok && CALL(f_read,u32,(void*,u32,u32,void*))(forge_records,1,size,file)==size;
        u8 extra;ok=ok && !CALL(f_read,u32,(void*,u32,u32,void*))(&extra,1,1,file) && !CALL(f_error,int,(void*))(file);
        int closed=CALL(f_close,int,(void*))(file);
        if(!ok || closed || h[3]!=checksum((const u8*)forge_records,size))fail("Card Forge: definition registry corrupt; registry and game save retained");
        forge_count=h[2];
        for(u32 i=0;i<forge_count;++i)if(!forge_record_valid(&forge_records[i]) || !forge_blueprint(forge_records[i].base))
            fail("Card Forge: invalid required definition; registry and game save retained");
    } else if(*CALL(f_errno,int*,(void))()!=2)fail("Card Forge: cannot read definitions; game save retained");
    u32 seen[3]={0};
    for(u32 i=0;i<forge_count;++i){
        u32 kind=forge_records[i].base>=28000000u?2:forge_records[i].base>=27000000u;
        forge_ids[i]=26000000u+kind*1000000u+forge_group_counts[kind]+seen[kind]++;
#ifdef GUEST_FORGE_SPELLS
        if(kind==2)forge_spell_slots[forge_spell_count++]=i;
        else forge_unit_slots[forge_unit_count++]=i;
#endif
    }
    forge_loaded_count=forge_count;forge_ready=1;
}
static int forge_finish(void *file) {
    int ok=!CALL(f_error,int,(void*))(file) && !CALL(f_flush,int,(void*))(file);
    int fd=CALL(f_fileno,int,(void*))(file);ok=ok && fd>=0 && !CALL(f_sync,int,(int))(fd);
    int closed=CALL(f_close,int,(void*))(file);return ok && !closed;
}
static int forge_disk_matches(void) {
    void *file=CALL(f_open,void*,(const char*,const char*))(forge_path,"rb");
    if(!file)return !forge_count && *CALL(f_errno,int*,(void))()==2;
    u32 h[5];int ok=CALL(f_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
    ok=ok && forge_header_valid(h) && h[2]==forge_count && h[3]==checksum((const u8*)forge_records,forge_count*sizeof *forge_records);
    for(u32 i=0;ok && i<forge_count;++i) {
        struct ForgeRecord row;ok=CALL(f_read,u32,(void*,u32,u32,void*))(&row,1,sizeof row,file)==sizeof row;
        for(u32 b=0;ok && b<sizeof row;++b)if(((const u8*)&row)[b]!=((const u8*)&forge_records[i])[b])ok=0;
    }
    u8 extra;ok=ok && !CALL(f_read,u32,(void*,u32,u32,void*))(&extra,1,1,file) && !CALL(f_error,int,(void*))(file);
    int closed=CALL(f_close,int,(void*))(file);return ok && !closed;
}
static int forge_append(u32 base,const char *name) {
    forge_initialize();
    if(forge_count>=FORGE_MAX_CARDS || !forge_blueprint(base) || !name || !forge_disk_matches())return 0;
    u32 bytes=0;while(bytes<=96 && name[bytes])++bytes;
    if(!forge_name_valid(name,bytes))return 0;
    struct ForgeRecord *r=&forge_records[forge_count];memset(r,0,sizeof *r);r->base=base;r->bytes=bytes;memcpy(r->name,name,bytes);
    u32 size=(forge_count+1)*sizeof *forge_records;
    u32 h[5]={0x31524746u,1,forge_count+1,checksum((const u8*)forge_records,size),0};h[4]=checksum((const u8*)h,16);
    char temp[520];path_append(temp,forge_path,".tmp",sizeof temp);
    void *file=CALL(f_open,void*,(const char*,const char*))(temp,"wb");if(!file)return 0;
    int ok=CALL(f_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
    ok=ok && CALL(f_write,u32,(const void*,u32,u32,void*))(forge_records,1,size,file)==size;
    int finished=forge_finish(file);
    if(!ok || !finished || CALL(f_rename,int,(const char*,const char*))(temp,forge_path))return 0;
    ++forge_count; // loaded prefix stays immutable until the next process.
    return 1;
}
static void forge_generate(const char *bundle_prefix) {
    if(forge_generated || !forge_loaded_count)return;
#ifdef GUEST_ART_SOURCE_TOKEN
    art_initialize();
#endif
    if(CALL(symbol("mkdir"),int,(const char*,int))(forge_cache,0700) && *CALL(f_errno,int*,(void))()!=17)
        fail("Card Forge: cannot create table cache; game save retained");
    for(u32 table=0;table<sizeof forge_table_paths/sizeof *forge_table_paths;++table) {
        char source[768],dest[768],temp[780];path_append(source,bundle_prefix,forge_table_paths[table],sizeof source);
        const char *leaf=forge_table_paths[table];for(const char *p=leaf;*p;++p)if(*p=='/')leaf=p+1;
        char suffix[80];path_append(suffix,"/",leaf,sizeof suffix);path_append(dest,forge_cache,suffix,sizeof dest);path_append(temp,dest,".tmp",sizeof temp);
        u32 size=forge_source_sizes[table];if(!size || size>2097152u)fail("Card Forge: source table exceeds bound");
        u8 *original=CALL(API_malloc,u8*,(u32))(size);if(!original)fail("Card Forge: table allocation failed");
        void *file=CALL(f_open,void*,(const char*,const char*))(source,"rb");if(!file)fail("Card Forge: original table missing; game save retained");
        int ok=CALL(f_read,u32,(void*,u32,u32,void*))(original,1,size,file)==size;u8 extra;
        ok=ok && !CALL(f_read,u32,(void*,u32,u32,void*))(&extra,1,1,file) && !CALL(f_error,int,(void*))(file);
        int closed=CALL(f_close,int,(void*))(file);
        if(!ok || closed || checksum(original,size)!=forge_source_hashes[table])fail("Card Forge: pinned table mismatch; original assets and save retained");
        file=CALL(f_open,void*,(const char*,const char*))(temp,"wb");if(!file)fail("Card Forge: cannot create table cache");
        ok=CALL(f_write,u32,(const void*,u32,u32,void*))(original,1,size,file)==size;
        if(original[size-1]!='\n')ok=ok && CALL(f_write,u32,(const void*,u32,u32,void*))("\r\n",1,2,file)==2;
        CALL(f_free,void,(void*))(original);
        for(u32 slot=0;slot<forge_loaded_count;++slot) {
            const struct ForgeBlueprint *bp=forge_blueprint(forge_records[slot].base);u32 bytes=0;
            const char *row=bp?bp->rows[table]:0;
#ifdef GUEST_ART_SOURCE_TOKEN
            #ifdef GUEST_FORGE_SPELLS
            if(bp && forge_records[slot].base>=28000000u){
                if(table==6 && (art_flags[slot]&1))row=forge_spell_icons[(bp-forge_blueprints)-GUEST_MOD_UNIT_COUNT];
            }else
#endif
            if(bp && table<4 && art_flags[slot]){
                u32 index=0;while(index<GUEST_MOD_UNIT_COUNT && forge_blueprints[index].id!=forge_records[slot].base)++index;
                if(index>=GUEST_MOD_UNIT_COUNT)fail("Card Forge: custom art blueprint missing");
                row=forge_art_templates[index][art_flags[slot]][table];
            }
#endif
            if(!bp || !forge_expand(forge_render,sizeof forge_render,row,&forge_records[slot],slot,&bytes))fail("Card Forge: row expansion failed");
            ok=ok && CALL(f_write,u32,(const void*,u32,u32,void*))(forge_render,1,bytes,file)==bytes;
        }
        int finished=forge_finish(file);
        if(!ok || !finished || CALL(f_rename,int,(const char*,const char*))(temp,dest))fail("Card Forge: cache commit failed; game save retained");
    }
    forge_generated=1;CALL(API_printf,int,(const char*,u32))("Card Forge: loaded %u immutable definitions\n",forge_loaded_count);
}
static void forge_prepare_tables(void) {
    forge_initialize();if(!forge_loaded_count || forge_generated)return;
    u32 msg=symbol("objc_msgSend"),sel=symbol("sel_registerName");
    u32 cls=CALL(symbol("objc_getClass"),u32,(const char*))("NSBundle");
    u32 bundle=CALL(msg,u32,(u32,u32))(cls,CALL(sel,u32,(const char*))("mainBundle"));
    u32 path=CALL(msg,u32,(u32,u32))(bundle,CALL(sel,u32,(const char*))("bundlePath"));
    const char *bytes=CALL(msg,const char*,(u32,u32))(path,CALL(sel,u32,(const char*))("UTF8String"));
    if(!bytes || !*bytes)fail("Card Forge: bundle path unavailable; game save retained");
    char prefix[768];path_append(prefix,bytes,"/",sizeof prefix);forge_generate(prefix);
}
void *guest_forge_fopen(const char *path,const char *mode) {
    if(path && mode && (forge_equal(mode,"r") || forge_equal(mode,"rb"))) {
#ifdef GUEST_MUSIC_LAB
        int music_handled=0;void *music_csv=music_csv_redirect(path,mode,&music_handled);if(music_handled)return music_csv;
#endif
        u32 length=forge_length(path);
#ifdef GUEST_ART_SOURCE_TOKEN
        forge_initialize();art_initialize();int handled=0;void *art=art_redirect(path,mode,&handled);if(handled)return art;
#endif
        for(u32 i=0;i<sizeof forge_table_paths/sizeof *forge_table_paths;++i) {
            u32 n=forge_length(forge_table_paths[i]);
            if(length<n || (length>n && path[length-n-1]!='/') || !forge_equal(path+length-n,forge_table_paths[i]))continue;
            forge_initialize();if(!forge_loaded_count)break;
            char prefix[768];u32 at=length-n;if(at>=sizeof prefix)fail("Card Forge: bundle path exceeds bound");
            memcpy(prefix,path,at);prefix[at]=0;forge_generate(prefix);
            const char *leaf=forge_table_paths[i];for(const char *p=leaf;*p;++p)if(*p=='/')leaf=p+1;
            char suffix[80],dest[768];path_append(suffix,"/",leaf,sizeof suffix);path_append(dest,forge_cache,suffix,sizeof dest);
            return CALL(f_open,void*,(const char*,const char*))(dest,mode);
        }
    }
    return CALL(symbol("fopen"),void*,(const char*,const char*))(path,mode);
}
static void write_forge_link(void *file,u32 save_hash) {
    if(!forge_loaded_count)return;
    u32 h[5]={0x31434746u,forge_loaded_count,checksum((const u8*)forge_records,forge_loaded_count*sizeof *forge_records),save_hash,0};h[4]=checksum((const u8*)h,16);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h)fail("Card Forge: cannot checkpoint definition link");
}
static void read_forge_link(void *file,u32 tag,u32 save_hash) {
    u32 h[5]={tag,0,0,0,0};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,16,file)!=16 || !forge_link_valid(h,save_hash,forge_records,forge_loaded_count))
        fail("Card Forge: required definitions missing or changed; game save retained");
}
