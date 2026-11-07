#include "mods_math.inc"
struct ModRecord {u32 value[MOD_FIELDS];};
static struct ModRecord mod_records[GUEST_MOD_UNIT_COUNT];
static void *mod_units[GUEST_MOD_UNIT_COUNT],*mod_shots[GUEST_MOD_UNIT_COUNT];
static u32 mod_original[GUEST_MOD_UNIT_COUNT][6],mod_enabled,mod_ready,mod_selected,mod_applied;
static int mod_profile_rejected,mod_reset_requested;
static char mod_status[128],mod_path[512];
static const u32 mod_offsets[6]={0x150,0x154,0x1bc,0x1cc,0x74,0x15c};
static void mod_message(const char *s) {u32 n=0;while(s[n]&&n<127){mod_status[n]=s[n];++n;}mod_status[n]=0;}
static void mod_reject_profile(void) {
    for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i)for(u32 f=0;f<MOD_FIELDS;++f)mod_records[i].value[f]=100;
    mod_enabled=mod_selected=0;mod_ready=mod_profile_rejected=1;
    mod_message("Saved modifiers rejected. Reset draft.");
    CALL(API_puts,int,(const char*))("Mod Lab: optional profile rejected; retained, mods disabled");
}
static int mod_allowed(void) {return save_initialized && logged_in && committed_wins>=7;}
static int mod_effective(void) {return mod_ready && mod_enabled && mod_allowed() && (!battle_started || arena_battle);}
static void mod_restore(void) {
    mod_applied=0;
    if(!mod_ready)return;
    for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i)for(u32 j=0;j<6;++j) {
        void *p=j==4?mod_shots[i]:mod_units[i];if(p)FIELD(p,mod_offsets[j])=mod_original[i][j];
    }
}
static void mod_apply_units(void) {
    mod_restore();mod_applied=1;if(!mod_effective())return;
    for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i)for(u32 j=0;j<6;++j) {
        u32 field=j==5?MOD_RANGE:j;void *p=j==4?mod_shots[i]:mod_units[i];
        if(p && mod_records[i].value[field]!=100)
            FIELD(p,mod_offsets[j])=mod_scaled(mod_original[i][j],mod_records[i].value[field],j==1||j==2);
    }
}
static void *mod_data(const char *name,u32 address) {
    if(!*name)return 0;u32 text[5];CALL(0x18dcf9,void,(void*,const char*))(text,name);
    void *p=CALL(address,void*,(void*,void*))(text,0);CALL(0x18db2d,void,(void*))(text);return p;
}
static void mod_initialize(void) {
    if(mod_ready)return;
    for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i) {
        for(u32 f=0;f<MOD_FIELDS;++f)mod_records[i].value[f]=100;
        void *spell=CALL(0xb821d,void*,(int))((int)mod_unit_defs[i].cardId);
        mod_units[i]=spell?CALL(0x75399,void*,(void*))(spell):0;
        mod_shots[i]=mod_data(mod_unit_defs[i].projectile,0xba141);
        if(!mod_units[i])fail("Mod Lab: pinned unit reference missing");
        for(u32 j=0;j<6;++j){void *p=j==4?mod_shots[i]:mod_units[i];mod_original[i][j]=p?FIELD(p,mod_offsets[j]):0;}
    }
    const char *home=CALL(s_getenv,const char*,(const char*))("HOME");path_append(mod_path,home,"/Documents/ClashModLab-v1.bin",sizeof mod_path);
    void *file=CALL(s_open,void*,(const char*,const char*))(mod_path,"rb");
    if(file) {
        u32 h[7];int ok=CALL(s_read,u32,(void*,u32,u32,void*))(h,1,sizeof h,file)==sizeof h;
        ok=ok && h[0]==0x31444f4du && h[1]==1 && h[2]>0 && h[2]<=GUEST_MOD_UNIT_COUNT && h[3]<=1 && h[4]<h[2] && h[6]==checksum((const u8*)h,24);
        u32 bytes=ok?h[2]*sizeof *mod_records:0;
        ok=ok && CALL(s_read,u32,(void*,u32,u32,void*))(mod_records,1,bytes,file)==bytes;
        u8 extra;ok=ok && !CALL(s_read,u32,(void*,u32,u32,void*))( &extra,1,1,file);
        ok=ok && !CALL(symbol("ferror"),int,(void*))(file);
        int closed=CALL(s_close,int,(void*))(file);if(!ok || closed || h[5]!=checksum((const u8*)mod_records,bytes)){mod_reject_profile();return;}
        for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i)for(u32 f=0;f<MOD_FIELDS;++f)
            if(!mod_percent_valid(f,mod_records[i].value[f])){mod_reject_profile();return;}
        mod_enabled=h[3];mod_selected=h[4];
    } else if(*CALL(s_error,int*,(void))()!=2){mod_reject_profile();return;}
    mod_ready=1;mod_message("Changes apply to the next battle.");
}
static int mod_archive_rejected(void) {
    if(!mod_profile_rejected)return 1;
    if(!mod_reset_requested){mod_message("Saved modifiers rejected. Reset draft before Apply.");return 0;}
    char rejected[520];path_append(rejected,mod_path,".rejected",sizeof rejected);
    void *f=CALL(s_open,void*,(const char*,const char*))(rejected,"rb");
    if(f){CALL(s_close,int,(void*))(f);mod_message("A rejected profile is already retained. Back it up first.");return 0;}
    if(*CALL(s_error,int*,(void))()!=2 || CALL(s_rename,int,(const char*,const char*))(mod_path,rejected)) {
        mod_message("Cannot archive rejected modifiers. Profile retained.");return 0;
    }
    mod_profile_rejected=0;return 1;
}
static void mod_save(void) {
    char temp[520];path_append(temp,mod_path,".tmp",sizeof temp);
    u32 h[7]={0x31444f4du,1,GUEST_MOD_UNIT_COUNT,mod_enabled,mod_selected,checksum((const u8*)mod_records,sizeof mod_records),0};h[6]=checksum((const u8*)h,24);
    void *f=CALL(s_open,void*,(const char*,const char*))(temp,"wb");if(!f)fail("Mod Lab: cannot open profile checkpoint");
    int ok=CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h &&
        CALL(s_write,u32,(const void*,u32,u32,void*))(mod_records,1,sizeof mod_records,f)==sizeof mod_records &&
        CALL(s_flush,int,(void*))(f)==0;
    int fd=CALL(s_fileno,int,(void*))(f);ok=ok && fd>=0 && !CALL(s_sync,int,(int))(fd);
    int closed=CALL(s_close,int,(void*))(f);
    if(!ok || closed || CALL(s_rename,int,(const char*,const char*))(temp,mod_path))fail("Mod Lab: profile commit failed");
}
static void mod_backup(void) {
    char backup[520];path_append(backup,save_path,".before-mods",sizeof backup);
    void *f=CALL(s_open,void*,(const char*,const char*))(backup,"rb");if(f){CALL(s_close,int,(void*))(f);return;}
    if(*CALL(s_error,int*,(void))()!=2)fail("Mod Lab: cannot inspect pre-mod backup");
    void *source=CALL(s_open,void*,(const char*,const char*))(save_path,"rb");
    char temp[528];path_append(temp,backup,".tmp",sizeof temp);
    f=CALL(s_open,void*,(const char*,const char*))(temp,"wb");if(!source || !f)fail("Mod Lab: backup unavailable");
    u8 buffer[4096];u32 n,total=0;int ok=1;
    while((n=CALL(s_read,u32,(void*,u32,u32,void*))(buffer,1,sizeof buffer,source))) {
        total+=n;if(total>1048576u)fail("Mod Lab: save exceeds backup bound");
        if(CALL(s_write,u32,(const void*,u32,u32,void*))(buffer,1,n,f)!=n){ok=0;break;}
    }
    ok=ok && !CALL(symbol("ferror"),int,(void*))(source);
    int source_closed=CALL(s_close,int,(void*))(source);
    ok=ok && !CALL(s_flush,int,(void*))(f);int fd=CALL(s_fileno,int,(void*))(f);
    ok=ok && fd>=0 && !CALL(s_sync,int,(int))(fd);int closed=CALL(s_close,int,(void*))(f);
    if(!ok || source_closed || closed || CALL(s_rename,int,(const char*,const char*))(temp,backup))fail("Mod Lab: backup commit failed");
}
static int mod_home(void **home,void **avatar) {
    if(!mod_allowed() || battle_started || pending_size || initial_name_pending || friendly_state==1 || friendly_state==2)return 0;
    void *ui=CALL(0x91f7d,void*,(void))();if(!ui || FIELD(ui,0x28)!=0)return 0;
    void *logic=(void*)FIELD(ui,0x54);if(!logic)return 0;
    *home=(void*)FIELD(logic,0x30);*avatar=(void*)FIELD(logic,0x34);
    return *home && *avatar && *home==delivered_home && logic==home_clock_logic && !BYTE(*home,0xb4);
}
static int mod_currency(u32 gold,u32 gems) {
    void *home,*avatar;if(!mod_home(&home,&avatar)){mod_message("Return home and finish pending rewards first.");return 0;}
    if(gold>1000000 || gems>1000000){mod_message("Enter a value from 0 to 1000000.");return 0;}
    if((int)gold>CALL(0x9ade5,int,(void*))(avatar)){mod_message("Gold exceeds this client's native wallet limit.");return 0;}
    capture_native(home,avatar);mod_backup();
    int old_gold=CALL(0x9ab55,int,(void*))(avatar),old_gems=CALL(0x9a41d,int,(void*))(avatar);
    if((int)gold>old_gold)CALL(0x9a569,void,(void*,int,int))(avatar,(int)gold-old_gold,0);
    else if((int)gold<old_gold)CALL(0x9a5ed,int,(void*,int,void*,int,int))(avatar,0,CALL(0xb8309,void*,(void))(),(int)gold-old_gold,0);
    if((int)gems>old_gems)CALL(0x9a531,void,(void*,int,int))(avatar,(int)gems-old_gems,0);
    else if((int)gems<old_gems)CALL(0x9ad31,void,(void*,int,void*))(avatar,old_gems-(int)gems,home);
    if(CALL(0x9ab55,int,(void*))(avatar)!=(int)gold || CALL(0x9a41d,int,(void*))(avatar)!=(int)gems)
        fail("Mod Lab: native currency change did not match request");
    capture_native(home,avatar);mod_message("Balances saved. Pre-mod save retained.");return 1;
}
extern int guest_original_mod_health(void*,int),guest_original_mod_damage(void*,int),guest_original_mod_shot_damage(void*,int);
static int mod_stat(void *data,int value,int field,int shot) {
    if(!mod_effective() || value<=0)return value;
    for(u32 i=0;i<GUEST_MOD_UNIT_COUNT;++i)if((shot?mod_shots[i]:mod_units[i])==data && mod_records[i].value[field]!=100)
        return (int)mod_scaled((u32)value,mod_records[i].value[field],0);
    return value;
}
int guest_mod_health(void *p,int level){return mod_stat(p,guest_original_mod_health(p,level),MOD_HEALTH,0);}
int guest_mod_damage(void *p,int level){return mod_stat(p,guest_original_mod_damage(p,level),MOD_DAMAGE,0);}
int guest_mod_shot_damage(void *p,int level){return mod_stat(p,guest_original_mod_shot_damage(p,level),MOD_DAMAGE,1);}
