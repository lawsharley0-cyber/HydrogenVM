#include "shop_math.inc"
#include "shop_view.inc"
static int shop_native_valid(void *home,void *avatar,int fresh) {
    u32 count=FIELD(home,0xe0),seed=FIELD(home,0xc0);
    if(!shop_fields_valid(seed,(int)FIELD(home,0xc4),count))return 0;
    if(!count)return 1; // Original training/legacy shop remains untouched.
    void *arena=CALL(0x9b4d1,void*,(void*))(avatar);
    void *globals=CALL(0xb81b9,void*,(void))();
    void *rarities[3]={CALL(0xba71d,void*,(void))(),CALL(0xba70d,void*,(void))(),CALL(0xba6fd,void*,(void))()};
    u32 limits[3]={FIELD(globals,0x194),FIELD(globals,0x190),FIELD(globals,0x18c)},seen[3]={0};
    void *slots=(void*)FIELD(home,0xd8);if(!slots || !arena)return 0;
    for(u32 i=0;i<count;++i) {
        void *slot=(void*)FIELD(slots,4*i);if(!slot)return 0;
        void *data=(void*)FIELD(slot,0);if(!data)return 0;
        int id=CALL(0x7d2e1,int,(void*))(data);
        if(id<26000000 || id>=29000000)return 0; // Before card-specific getters.
        for(u32 j=0;j<i;++j)if(FIELD((void*)FIELD(slots,4*j),0)==(u32)data)return 0;
        if(CALL(0x753ad,int,(void*))(data))return 0;
        // Eligibility belongs to generation. A later arena demotion must not
        // invalidate an already offered card or its paid purchase history.
        if(fresh && !CALL(0x755fd,int,(void*,void*))(data,arena))return 0;
        void *rarity=CALL(0x753b5,void*,(void*))(data);int kind=-1;
        for(int k=0;k<3;++k)if(rarity==rarities[k])kind=k;
        if(kind<0 || !limits[kind] || limits[kind]>1000 || FIELD(slot,4)>limits[kind])return 0;
        ++seen[kind];
    }
    u32 each=FIELD(home,0xc4)==1?2u:1u;
    return seen[0]==each && seen[1]==each && seen[2]==each;
}
static void validate_shop_checkpoint(void) {
    // Run before every login migration/seeding/write, using the original
    // decoder but no game-mode association or shop mutation.
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30);
    void *avatar=(void*)FIELD(owner,0x34);
    int valid=home && avatar && shop_native_valid(home,avatar,0);
    dispose(owner);
    if(!valid)fail("GuestOffline: invalid saved shop calendar/items; original retained");
}
static void shop_refresh(void *logic,void *home,u32 now) {
    void *avatar=(void*)FIELD(logic,0x34);
    if(!avatar || pending_size || initial_name_pending || friendly_state==1 || friendly_state==2)return;
    void *arena=CALL(0x9b4d1,void*,(void*))(avatar);
    if(!arena || !shop_eligible((int)FIELD(avatar,0x9c),CALL(0x7d2e1,int,(void*))(arena)))return;
    u32 seed=FIELD(home,0xc0),count=FIELD(home,0xe0);
    int change=shop_should_refresh(seed,now);
    if(change<0 || !shop_fields_valid(seed,(int)FIELD(home,0xc4),count))
        fail("GuestOffline: native shop calendar/item count mismatch; original retained");
    if(!change)return; // Includes a backward calendar: never reroll paid items.
    u32 day=shop_day(now),weekday=shop_weekday(day),seconds=shop_seconds_left(now);
    CALL(0x1269a9,void,(void*,int,int,int))(home,GUEST_SHOP_SEED_BASE+day,seconds,weekday);
    count=FIELD(home,0xe0);
    if(!shop_native_valid(home,avatar,1) || FIELD(home,0xc0)!=GUEST_SHOP_SEED_BASE+day || FIELD(home,0xc4)!=weekday)
        fail("GuestOffline: native shop refresh did not retain its calendar/items");
    shop_refresh_view((int)weekday);
    CALL(API_printf,int,(const char*,u32,u32,u32,u32))(
        "GuestOffline: original daily shop day=%u weekday=%u items=%u seconds=%u\n",day,weekday,count,seconds);
}
