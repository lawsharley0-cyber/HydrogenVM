static int opponent_avatar(void *avatar) {
    return opponent_enabled && avatar && FIELD(avatar,0x10)==0xffffffffu && FIELD(avatar,0x14)==0;
}
static void opponent_assign_name(void *name) {
    u32 text[5];
    CALL(0x18dcf9,void,(void*,const char*))(text,opponent_names[opponent_history.name]);
    CALL(0x18df31,void*,(void*,void*))(name,text);
    CALL(0x18db2d,void,(void*))(text);
}
static void *opponent_avatar_name(void *avatar) {
    void *name=(u8*)avatar+0x20;
    if(opponent_avatar(avatar)) {
        const char *actual=CALL(0x19c099,const char*,(void*))(name);
        const char *wanted=opponent_names[opponent_history.name];
        u32 n=0;while(wanted[n])++n;
        int equal=CALL(0x18f4bd,u32,(void*))(name)==n && actual;
        if(equal)for(u32 i=0;i<n;++i)if(actual[i]!=wanted[i]){equal=0;break;}
        // Native NPC UI paths can restore the CSV trainer label. Keep the
        // actual avatar String consistent, rather than returning a UI alias.
        if(!equal)opponent_assign_name(name);
    }
    return name;
}
void *guest_avatar_set_name(void *avatar,void *value) {
    void *name=(u8*)avatar+0x20;
    if(opponent_avatar(avatar)) {opponent_assign_name(name);return name;}
    return CALL(0x18df31,void*,(void*,void*))(name,value);
}
static int opponent_applies(void *npc) {
    return opponent_enabled && npc && (int)CALL(0x7d2e1,int,(void*))(npc)==arena1_missions[opponent_history.variant];
}
void *guest_npc_label(void *npc) {
    if(!opponent_applies(npc))return CALL(0x511a9,void*,(void*))(npc);
    void *mode=CALL(0x91f7d,void*,(void))();
    void *avatar=CALL(0x93e8d,void*,(void*))(mode);
    if(!opponent_avatar(avatar))fail("GuestOffline: selected NPC label identity mismatch");
    return opponent_avatar_name(avatar);
}
static int opponent_rate(void *npc,int phase,u32 column) {
    if(!opponent_applies(npc))return CALL(0x19120d,int,(void*,int))((void*)FIELD(npc,4),(int)FIELD((void*)column,0));
    // These are the pinned player's original full-pool regeneration rates.
    // Native summoner arithmetic still divides by MAX_MANA and pays card cost.
    static u32 logged[3];
    if(logged[phase]!=opponent_history.serial) {
        logged[phase]=opponent_history.serial;
        CALL(API_printf,int,(const char*,int,int))("GuestOffline: native fair AI mana phase=%d full_pool_ms=%d\n",phase,arena_mana_rates[phase]);
    }
    return arena_mana_rates[phase];
}
int guest_npc_mana(void *npc) {return opponent_rate(npc,0,0x464c98);}
int guest_npc_mana_end(void *npc) {return opponent_rate(npc,1,0x464c9c);}
int guest_npc_mana_overtime(void *npc) {return opponent_rate(npc,2,0x464ca0);}
int guest_npc_reserve(void *npc) {
    if(!opponent_applies(npc))return (int)FIELD(npc,0x50);
    // Reserve is retained after spending, not merely a threshold to start a
    // push. Keep even the patient profile able to afford every authored card.
    static const int reserves[3]={0,2,4};static u32 logged;
    if(logged!=opponent_history.serial) {
        logged=opponent_history.serial;
        CALL(API_printf,int,(const char*,u32,int))("GuestOffline: native AI reserve style=%u reserve=%d\n",opponent_history.style,reserves[opponent_history.style]);
    }
    // The caller retains its original low-HP, first14s and late-battle rules;
    // threat heatmaps, spell decisions, legal placement and elixir costs remain native.
    return reserves[opponent_history.style];
}
