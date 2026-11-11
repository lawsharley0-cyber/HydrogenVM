/* Optional MCH1 profile; native references resolved afresh, never persisted. */
#include "mechanics_rules.inc"
#define MECH_CAPACITY (GUEST_MECH_CARD_COUNT+FORGE_MAX_CARDS)
struct MechNative {void *spell,*unit,*impact;u32 original[12];};
static struct MechNative mech_native[MECH_CAPACITY];
static struct MechRecord mech_records[MECH_CAPACITY];
static void *mech_troops[GUEST_MECH_TROOP_COUNT],*mech_projectiles[GUEST_MECH_SHOT_COUNT];
static u32 mech_shot_speeds[GUEST_MECH_SHOT_COUNT];
static u32 mech_ready,mech_enabled,mech_selected;
static int mech_rejected,mech_reset_requested;
static char mech_path[512];
static const u32 mech_offsets[12]={0x7c,0x1ac,0xb4,0xbc,0x64,0x68,0x58,0x50,0x88,0x1b0,0x1c0,0x74};
static const u8 mech_roles[12]={0,1,1,1,1,1,2,2,2,1,1,0};
static u32 mech_count(void){return GUEST_MECH_CARD_COUNT+forge_loaded_count;}
static u32 mech_id(u32 i){return i<GUEST_MECH_CARD_COUNT?mech_cards[i].id:forge_ids[i-GUEST_MECH_CARD_COUNT];}
static const char *mech_name(u32 i){return i<GUEST_MECH_CARD_COUNT?mech_cards[i].name:forge_records[i-GUEST_MECH_CARD_COUNT].name;}
static void *mech_object(u32 i,u32 prop){struct MechNative *n=&mech_native[i];return mech_roles[prop]==0?n->spell:mech_roles[prop]==1?n->unit:n->impact;}
static void mech_write(u32 i,u32 prop,u32 value){void *p=mech_object(i,prop);if(p)FIELD(p,mech_offsets[prop])=value;}
static int mech_supported(u32 i,u32 field){
    struct MechNative *n=&mech_native[i];
    if(field==MECH_COUNT || field==MECH_DELAY)return n->unit!=0;
    if(field==MECH_SPAWN_COUNT || field==MECH_SPAWN_TYPE || field==MECH_PAUSE)return n->unit && n->original[2]!=0;
    if(field==MECH_SHOT)return n->unit && n->original[4]!=0;
    if(field==MECH_IMPACT_COUNT || field==MECH_IMPACT_TYPE)return n->impact && n->original[7]!=0;
    return field==MECH_ELIXIR;
}
static int mech_record_valid(u32 i,const struct MechRecord *r){
    if(r->id!=mech_id(i))return 0;
    for(u32 f=0;f<MECH_FIELDS;++f)if(!mech_value_valid(f,r->value[f],GUEST_MECH_TROOP_COUNT,GUEST_MECH_SHOT_COUNT) || (r->value[f] && !mech_supported(i,f)))return 0;
    return 1;
}
static void mech_reject(void){
    for(u32 i=0;i<mech_count();++i){memset(&mech_records[i],0,sizeof *mech_records);mech_records[i].id=mech_id(i);}
    mech_enabled=mech_selected=0;mech_rejected=1;
    CALL(API_puts,int,(const char*))("Mod Lab: mechanics profile rejected; file retained, mechanics disabled");
}
static void mech_initialize(void){
    if(mech_ready)return;
    for(u32 i=0;i<GUEST_MECH_TROOP_COUNT;++i){mech_troops[i]=mod_data(mech_troop_names[i],0xb9e9d);if(!mech_troops[i])fail("Mod Lab: missing spawn choice");}
    for(u32 i=0;i<GUEST_MECH_SHOT_COUNT;++i){mech_projectiles[i]=mod_data(mech_shot_names[i],0xba141);if(!mech_projectiles[i])fail("Mod Lab: missing projectile choice");mech_shot_speeds[i]=FIELD(mech_projectiles[i],0x74);}
    for(u32 i=0;i<mech_count();++i){
        struct MechNative *n=&mech_native[i];n->spell=CALL(0xb821d,void*,(int))((int)mech_id(i));
        if(!n->spell)fail("Mod Lab: missing mechanics card");
        n->unit=CALL(0x75399,void*,(void*))(n->spell);
        n->impact=CALL(0x7539d,void*,(void*))(n->spell);
        for(u32 p=0;p<12;++p){void *o=mech_object(i,p);n->original[p]=o?FIELD(o,mech_offsets[p]):0;}
        mech_records[i].id=mech_id(i);
    }
    const char *home=CALL(s_getenv,const char*,(const char*))("HOME");path_append(mech_path,home,"/Documents/ClashMechanics-v1.bin",sizeof mech_path);
    void *file=CALL(s_open,void*,(const char*,const char*))(mech_path,"rb");
    mech_ready=1;
    if(!file){if(*CALL(s_error,int*,(void))()!=2)mech_reject();return;}
    u32 h[7];int ok=CALL(s_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
    ok=ok && h[0]==0x3148434du && h[1]==1 && h[2]>0 && h[2]<=mech_count() && h[3]<=1 && h[4]<h[2] && h[6]==checksum((const u8*)h,24);
    u32 bytes=ok?h[2]*sizeof *mech_records:0;
    ok=ok && CALL(s_read,u32,(void*,u32,u32,void*))(mech_records,1,bytes,file)==bytes;
    u8 extra;ok=ok && !CALL(s_read,u32,(void*,u32,u32,void*))(&extra,1,1,file) && !CALL(symbol("ferror"),int,(void*))(file);
    int closed=CALL(s_close,int,(void*))(file);
    if(!ok || closed || h[5]!=checksum((const u8*)mech_records,bytes)){mech_reject();return;}
    for(u32 i=0;i<mech_count();++i)if(!mech_record_valid(i,&mech_records[i])){mech_reject();return;}
    mech_enabled=h[3];mech_selected=h[4];
}
static void mech_restore(void){
    if(!mech_ready)return;
    for(u32 i=0;i<GUEST_MECH_SHOT_COUNT;++i)if(mech_projectiles[i])FIELD(mech_projectiles[i],0x74)=mech_shot_speeds[i];
    for(u32 i=0;i<mech_count();++i)for(u32 p=0;p<12;++p)mech_write(i,p,mech_native[i].original[p]);
}
static void mech_apply(void){
    mech_initialize();mech_restore();
    if(!mech_enabled || !mod_allowed() || (battle_started && !arena_battle))return;
    for(u32 i=0;i<mech_count();++i){u32 *v=mech_records[i].value;
        if(v[MECH_COUNT])mech_write(i,0,v[MECH_COUNT]);
        if(v[MECH_SPAWN_COUNT])mech_write(i,1,v[MECH_SPAWN_COUNT]);
        if(v[MECH_SPAWN_TYPE]){mech_write(i,2,(u32)mech_troops[v[MECH_SPAWN_TYPE]-1]);mech_write(i,3,0);}
        if(v[MECH_SHOT]){mech_write(i,4,(u32)mech_projectiles[v[MECH_SHOT]-1]);mech_write(i,5,0);}
        if(v[MECH_IMPACT_COUNT])mech_write(i,6,v[MECH_IMPACT_COUNT]);
        if(v[MECH_IMPACT_TYPE]){mech_write(i,7,(u32)mech_troops[v[MECH_IMPACT_TYPE]-1]);mech_write(i,8,0);}
        if(v[MECH_PAUSE])mech_write(i,9,v[MECH_PAUSE]);
        if(v[MECH_DELAY])mech_write(i,10,v[MECH_DELAY]);
        if(v[MECH_ELIXIR])mech_write(i,11,v[MECH_ELIXIR]);
    }
}
static void *mech_active_shot(void *unit,void *fallback){
    return unit && FIELD(unit,0x64)?(void*)FIELD(unit,0x64):fallback;
}
static u32 mech_shot_base(void *shot,u32 fallback){
    for(u32 i=0;i<GUEST_MECH_SHOT_COUNT;++i)if(mech_projectiles[i]==shot)return mech_shot_speeds[i];
    return fallback;
}
static int mech_save(void){
    for(u32 i=0;i<mech_count();++i)if(!mech_record_valid(i,&mech_records[i])){mod_message("Unsupported mechanics choice. Nothing saved.");return 0;}
    if(mech_rejected){
        if(!mech_reset_requested){mod_message("Mechanics file rejected. Reset before Apply.");return 0;}
        char old[528];path_append(old,mech_path,".rejected",sizeof old);
        void *f=CALL(s_open,void*,(const char*,const char*))(old,"rb");if(f){CALL(s_close,int,(void*))(f);mod_message("A rejected mechanics file is already retained.");return 0;}
        if(*CALL(s_error,int*,(void))()!=2 || CALL(s_rename,int,(const char*,const char*))(mech_path,old)){mod_message("Cannot retain rejected mechanics file.");return 0;}
        mech_rejected=0;
    }
    char temp[528];path_append(temp,mech_path,".tmp",sizeof temp);
    u32 bytes=mech_count()*sizeof *mech_records;
    u32 h[7]={0x3148434du,1,mech_count(),mech_enabled,mech_selected,checksum((const u8*)mech_records,bytes),0};h[6]=checksum((const u8*)h,24);
    void *f=CALL(s_open,void*,(const char*,const char*))(temp,"wb");if(!f){mod_message("Cannot open mechanics checkpoint.");return 0;}
    int ok=CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h && CALL(s_write,u32,(const void*,u32,u32,void*))(mech_records,1,bytes,f)==bytes;
    int flushed=CALL(s_flush,int,(void*))(f),fd=CALL(s_fileno,int,(void*))(f);ok=ok && !flushed && fd>=0 && !CALL(s_sync,int,(int))(fd);
    int closed=CALL(s_close,int,(void*))(f);
    if(!ok || closed || CALL(s_rename,int,(const char*,const char*))(temp,mech_path)){mod_message("Mechanics checkpoint failed. Previous file retained.");return 0;}
    mod_apply_units();mod_message("Mechanics saved for the next battle.");return 1;
}
