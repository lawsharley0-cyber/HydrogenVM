/* Separate SLP1 optional profile: existing unit/mechanics/definition order stays fixed. */
#include "spells_rules.inc"
#ifdef GUEST_FORGE_SPELLS
#define SPELL_CAPACITY (GUEST_SPELL_COUNT+FORGE_MAX_CARDS)
static struct SpellDef spell_catalog[SPELL_CAPACITY];
static char spell_shot_names[FORGE_MAX_CARDS][24];
static u32 spell_count(void){return GUEST_SPELL_COUNT+forge_spell_count;}
static void spell_prepare_catalog(void){
    for(u32 i=0;i<GUEST_SPELL_COUNT;++i)spell_catalog[i]=spell_defs[i];
    for(u32 n=0;n<forge_spell_count;++n){
        u32 slot=forge_spell_slots[n],base=forge_records[slot].base,i=0;
        while(i<GUEST_SPELL_COUNT && spell_defs[i].id!=base)++i;
        if(i==GUEST_SPELL_COUNT)fail("Spell Lab: custom spell blueprint missing");
        struct SpellDef *d=&spell_catalog[GUEST_SPELL_COUNT+n];*d=spell_defs[i];
        d->id=forge_ids[slot];d->name=forge_records[slot].name;
        forge_identifier(spell_shot_names[n],"ModLabShot_",slot);d->shots[0]=spell_shot_names[n];
    }
}
#else
#define SPELL_CAPACITY GUEST_SPELL_COUNT
#define spell_catalog spell_defs
#define spell_count() GUEST_SPELL_COUNT
#endif
struct SpellNative {void *card,*area,*shots[3];u32 card_cost,card_radius,area_values[3],shot_values[3][2];};
static struct SpellNative spell_native[SPELL_CAPACITY];
static struct SpellRecord spell_records[SPELL_CAPACITY];
static u32 spell_ready,spell_enabled,spell_selected,spell_rejected,spell_reset_requested;
static char spell_path[512];
static void spell_defaults(void){
    for(u32 i=0;i<spell_count();++i){spell_records[i].id=spell_catalog[i].id;for(u32 f=0;f<SPELL_FIELDS;++f)spell_records[i].value[f]=spell_default(f);}
    spell_enabled=spell_selected=0;
}
static int spell_record_valid(u32 i,const struct SpellRecord *record){
    if(i>=spell_count() || record->id!=spell_catalog[i].id)return 0;
    for(u32 f=0;f<SPELL_FIELDS;++f)if(!spell_value_valid(f,record->value[f],spell_catalog[i].caps))return 0;return 1;
}
static void spell_initialize(void){
    if(spell_ready)return;spell_ready=1;
#ifdef GUEST_FORGE_SPELLS
    spell_prepare_catalog();
#endif
    spell_defaults();
    for(u32 i=0;i<spell_count();++i){struct SpellNative *n=&spell_native[i];
        n->card=CALL(0xb821d,void*,(int))((int)spell_catalog[i].id);if(!n->card)fail("Spell Lab: missing pinned card");
        n->card_cost=FIELD(n->card,0x74);n->card_radius=FIELD(n->card,0x84);n->area=CALL(0x753a5,void*,(void*))(n->card);
        if(n->area){n->area_values[0]=FIELD(n->area,0x74);n->area_values[1]=FIELD(n->area,0x6c);n->area_values[2]=FIELD(n->area,0x70);}
        for(u32 j=0;j<3;++j){n->shots[j]=mod_data(spell_catalog[i].shots[j],0xba141);if(*spell_catalog[i].shots[j] && !n->shots[j])fail("Spell Lab: missing pinned projectile");
            if(n->shots[j]){n->shot_values[j][0]=FIELD(n->shots[j],0x80);n->shot_values[j][1]=FIELD(n->shots[j],0x74);}}
    }
    path_append(spell_path,CALL(s_getenv,const char*,(const char*))("HOME"),"/Documents/ClashSpellLab-v1.bin",sizeof spell_path);
    void *file=CALL(s_open,void*,(const char*,const char*))(spell_path,"rb");int ok=1;
    if(file){u32 h[7];ok=CALL(s_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
        ok=ok && h[0]==0x31504c53u && h[1]==1 && h[2]>0 && h[2]<=spell_count() && h[3]<=1 && h[4]<h[2] && h[6]==checksum((const u8*)h,24);
        u32 bytes=ok?h[2]*sizeof *spell_records:0;ok=ok && CALL(s_read,u32,(void*,u32,u32,void*))(spell_records,1,bytes,file)==bytes;
        u8 extra;ok=ok && !CALL(s_read,u32,(void*,u32,u32,void*))(&extra,1,1,file) && !CALL(symbol("ferror"),int,(void*))(file);
        int closed=CALL(s_close,int,(void*))(file);ok=ok && !closed && h[5]==checksum((const u8*)spell_records,bytes);
        for(u32 i=0;ok && i<spell_count();++i)ok=spell_record_valid(i,&spell_records[i]);
        if(ok){spell_enabled=h[3];spell_selected=h[4];}
    }else ok=*CALL(s_error,int*,(void))()==2;
    if(!ok){spell_defaults();spell_rejected=1;CALL(API_puts,int,(const char*))("Spell Lab: rejected optional profile retained; original spells used");}
}
static int spell_effective(void){return spell_ready && spell_enabled && mod_allowed() && (!battle_started || arena_battle);}
static void spell_restore(void){
    if(!spell_ready)return;
    for(u32 i=0;i<spell_count();++i){struct SpellNative *n=&spell_native[i];
        FIELD(n->card,0x74)=n->card_cost;FIELD(n->card,0x84)=n->card_radius;
        if(n->area){FIELD(n->area,0x74)=n->area_values[0];FIELD(n->area,0x6c)=n->area_values[1];FIELD(n->area,0x70)=n->area_values[2];}
        for(u32 j=0;j<3;++j)if(n->shots[j]){FIELD(n->shots[j],0x80)=n->shot_values[j][0];FIELD(n->shots[j],0x74)=n->shot_values[j][1];}
    }
}
static void spell_apply(void){
    if(!spell_effective())return;
    for(u32 i=0;i<spell_count();++i){struct SpellNative *n=&spell_native[i];u32 *v=spell_records[i].value;
        if(v[SPELL_COST])FIELD(n->card,0x74)=v[SPELL_COST];
        if(v[SPELL_RADIUS]!=100){FIELD(n->card,0x84)=mod_scaled(n->card_radius,v[SPELL_RADIUS],0);if(n->area)FIELD(n->area,0x74)=mod_scaled(n->area_values[0],v[SPELL_RADIUS],0);}
        if(v[SPELL_DURATION]!=100 && n->area){FIELD(n->area,0x6c)=mod_scaled(n->area_values[1],v[SPELL_DURATION],0);FIELD(n->area,0x70)=mod_scaled(n->area_values[2],v[SPELL_DURATION],0);}
        for(u32 j=0;j<3;++j)if(n->shots[j]){if(v[SPELL_RADIUS]!=100)FIELD(n->shots[j],0x80)=mod_scaled(n->shot_values[j][0],v[SPELL_RADIUS],0);if(v[SPELL_SPEED]!=100)FIELD(n->shots[j],0x74)=mod_scaled(n->shot_values[j][1],v[SPELL_SPEED],0);}
    }
}
static int spell_damage(void *data,int raw,int *handled){
    if(spell_effective() && raw>0)for(u32 i=0;i<spell_count();++i)if(spell_records[i].value[SPELL_DAMAGE]!=100)
        for(u32 j=0;j<3;++j)if(data && data==spell_native[i].shots[j]){*handled=1;return (int)mod_scaled((u32)raw,spell_records[i].value[SPELL_DAMAGE],0);}
    return raw;
}
static int spell_save(void){
    for(u32 i=0;i<spell_count();++i)if(!spell_record_valid(i,&spell_records[i])){mod_message("Invalid spell profile. Nothing saved.");return 0;}
    char temp[530];path_append(temp,spell_path,".tmp",sizeof temp);
    if(spell_rejected){
        if(!spell_reset_requested){mod_message("Rejected spell profile retained. Reset first.");return 0;}
        char archive[540];path_append(archive,spell_path,".rejected",sizeof archive);void *old=CALL(s_open,void*,(const char*,const char*))(archive,"rb");
        if(old){CALL(s_close,int,(void*))(old);mod_message("Rejected archive exists. Back it up first.");return 0;}
        if(*CALL(s_error,int*,(void))()!=2 || CALL(s_rename,int,(const char*,const char*))(spell_path,archive)){mod_message("Cannot retain rejected spell profile.");return 0;}spell_rejected=0;
    }
    u32 bytes=spell_count()*sizeof *spell_records,h[7]={0x31504c53u,1,spell_count(),spell_enabled,spell_selected,checksum((const u8*)spell_records,bytes),0};h[6]=checksum((const u8*)h,24);
    void *f=CALL(s_open,void*,(const char*,const char*))(temp,"wb");if(!f){mod_message("Cannot open spell checkpoint.");return 0;}
    int ok=CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h && CALL(s_write,u32,(const void*,u32,u32,void*))(spell_records,1,bytes,f)==bytes;
    int flushed=CALL(s_flush,int,(void*))(f),fd=CALL(s_fileno,int,(void*))(f);ok=ok && !flushed && fd>=0 && !CALL(s_sync,int,(int))(fd);int closed=CALL(s_close,int,(void*))(f);
    if(!ok || closed || CALL(s_rename,int,(const char*,const char*))(temp,spell_path)){mod_message("Spell commit failed. Previous profile kept.");return 0;}
    mod_apply_units();mod_message("Spell settings saved for next battle.");return 1;
}
