/* Paid Friendly Challenge with an explicit simulated local NPC. The NPC has a
 * five-Gold bout escrow, accepts only after a durable pending interval, and uses
 * the original trainer deck/AI. No online player or acceptance is invented. */
extern int guest_original_challenge_execute(void*,void*,int,int);
#include "friendly_identity.inc"
static void friendly_migrate_identity(void) {
    if(!friendly_sequence || friendly_namespace==2)return;
    // Called only after every native/extension/wire guard passes. Legacy GFR1
    // shared (0,sequence) with chat. Normalize its opaque IDs atomically; the
    // historical debit, entry text and native result stay unchanged.
    friendly_identity_migration(friendly_entry,friendly_entry_size,friendly_result,friendly_result_size,&friendly_namespace);
    write_save();
    CALL(API_puts,int,(const char*))("GuestOffline: migrated validated friendly history to its own stream namespace");
}
static void friendly_native_log(void *battle) {
    void *mode=(void*)FIELD(battle,0);
    if(!mode)fail("GuestOffline: friendly native log has no game mode");
    if(FIELD(battle,0xb0))return;
    // Live sectors already contain both players and bypass LogicGameMode::
    // addPlayer. After native decode, populate the client's real log from those
    // decoded avatars/decks; native battleEnded supplies stars and score fields.
    void *log=CALL(API_malloc,void*,(u32))(0x1c);
    if(!log)fail("GuestOffline: friendly native log allocation failed");
    CALL(0x1c871,void,(void*))(log);FIELD(battle,0xb0)=(u32)log;
    for(int index=0;index<2;++index) {
        void *id=(void*)FIELD(battle,0xc+4*index);
        void *avatar=id?CALL(0xd7be9,void*,(void*,int,int))(mode,FIELD(id,0),FIELD(id,4)):0;
        void *player=CALL(0x1c8a9,void*,(void*,int))(log,index);
        void *deck=CALL(0x1491d,void*,(void*,int))(battle,index);
        if(!avatar || !player || !deck)fail("GuestOffline: incomplete native friendly log player");
        CALL(0x71b5d,void,(void*,void*))(player,CALL(0x99e31,void*,(void*))(avatar));
        void *alliance=CALL(0x9ae01,void*,(void*))(avatar);
        if(alliance)CALL(0x71b7d,void,(void*,void*))(player,alliance);
        CALL(0x71b6d,void,(void*,void*))(player,CALL(0x99e41,void*,(void*))(avatar));
        CALL(0x71bad,void,(void*,void*))(player,CALL(0x99e45,void*,(void*))(avatar));
        CALL(0x71b8d,void,(void*,int))(player,0);
        CALL(0x71bb9,void,(void*,void*))(player,CALL(0x9ae31,void*,(void*))(avatar));
        CALL(0x71bc9,void,(void*,void*))(player,CALL(0x9ae39,void*,(void*))(avatar));
        CALL(0x71ba5,void,(void*,int))(player,0);
        void *copy=CALL(0x163f1,void*,(void*))(deck);
        if(!copy)fail("GuestOffline: friendly native deck clone failed");
        CALL(0x7203d,void,(void*,void*))(player,copy);
    }
    CALL(API_printf,int,(const char*,u32,u32))("GuestOffline: native friendly log ready; type=%u challenge=%u\n",FIELD(battle,8),friendly_sequence);
}
static void friendly_stream_base(u32 *entry,void *avatar) {
    static u32 identity[2];
    identity[0]=friendly_namespace;identity[1]=friendly_sequence;
    FIELD(entry,4)=(u32)identity;
    FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(avatar);
    FIELD(entry,0xc)=FIELD(entry,8);
    FIELD(entry,0x10)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(avatar);
    FIELD(entry,0x18)=(u32)CALL(0x9ae41,int,(void*))(avatar);
}
static void friendly_encode_entry(void *avatar,void *message,int closed) {
    u32 stream[16],entry[12]={0};
    CALL(0x192a71,void,(void*,u32))(stream,2048);
    CALL(0x193749,void,(void*,int))(stream,10);
    friendly_stream_base(entry,avatar);FIELD(entry,0x24)=(u32)message;BYTE(entry,0x28)=closed!=0;
    CALL(0x1283dd,void,(void*,void*))(entry,stream);
    friendly_entry_size=CALL(0x192b2d,u32,(void*))(stream);
    if(friendly_entry_size>sizeof friendly_entry)fail("GuestOffline: friendly stream exceeds bound");
    const u8 *bytes=CALL(0x1936fd,const u8*,(void*))(stream);
    for(u32 i=0;i<friendly_entry_size;++i)friendly_entry[i]=bytes[i];
    CALL(0x19370d,void,(void*))(stream);
}
static void friendly_echo_payload_crc(void *command) {
    u32 stream[16];CALL(0x192a71,void,(void*,u32))(stream,1024);
    void *message=(void*)FIELD(command,0x18);
    if(message) {
        CALL(0x193749,void,(void*,int))(stream,1);
        CALL(0x192d0d,void,(void*,void*))(stream,message);
    } else CALL(0x193749,void,(void*,int))(stream,0);
    friendly_echo_crc=checksum(CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    CALL(0x19370d,void,(void*))(stream);
}
int guest_challenge_execute(void *command,void *logic,int mode,int flags) {
    if((mode&2) && save_initialized && home_clock_logic==logic) {
        void *home=CALL(0xd7355,void*,(void*))(logic);
        if(home==delivered_home)account_home_clock(logic,home,1);
    }
    int result=guest_original_challenge_execute(command,logic,mode,flags);
    if(result || !(mode&2))return result;
    void *home=CALL(0xd7355,void*,(void*))(logic),*avatar=CALL(0xd742d,void*,(void*))(logic);
    if(!save_initialized || !avatar || FIELD(avatar,0x10)!=0 || FIELD(avatar,0x14)!=1)return result;
    if(!home || home!=delivered_home || home_clock_logic!=logic || clan_status!=2 || pending_size ||
       friendly_state==1 || friendly_state==2 || friendly_sequence>=1000000000)
        fail("GuestOffline: native challenge completed outside a valid local transaction");
    int gold=CALL(0x9ab55,int,(void*))(avatar),free=CALL(0x9ab7d,int,(void*))(avatar);
    void *globals=CALL(0xb81b9,void*,(void))();
    int cost=CALL(0x63029,int,(void*))(globals);
    void *gold_data=CALL(0xb8309,void*,(void))();
    if(cost!=5 || !gold_data || gold<cost || free<0 || free>gold)
        fail("GuestOffline: invalid or insufficient friendly challenge balance");
    friendly_namespace=2;++friendly_sequence;friendly_gold_before=gold;friendly_free_before=free;
    CALL(0x9a5ed,int,(void*,int,void*,int,int))(avatar,0,gold_data,-cost,0);
    friendly_gold_after=CALL(0x9ab55,int,(void*))(avatar);
    friendly_free_after=CALL(0x9ab7d,int,(void*))(avatar);
    if(friendly_gold_after!=gold-cost || friendly_free_after!=(free<friendly_gold_after?free:friendly_gold_after))
        fail("GuestOffline: native friendly challenge accounting mismatch");
    friendly_encode_entry(avatar,(void*)FIELD(command,0x18),0);friendly_echo_payload_crc(command);
    friendly_state=1;friendly_stamp=wall_epoch();friendly_accepted_stamp=0;friendly_result_size=0;
    friendly_npc_before=5;friendly_npc_after=5;
    account_home_clock(logic,home,0);capture_native(home,avatar);
    if(!logged_in || !active_messaging || !BYTE(active_messaging,0xe4))
        fail("GuestOffline: local friendly conversation unavailable");
    reply(active_messaging,24312,0,friendly_entry,friendly_entry_size);
    CALL(API_printf,int,(const char*,u32,int))("GuestOffline: paid friendly challenge=%u gold=%d; local NPC pending\n",friendly_sequence,friendly_gold_after);
    return result;
}
static u32 prepare_friendly_sector_packet(void) {
    static u8 sector[65536];u32 size=assemble_sector_template(sector,sizeof sector,&npc_sectors[6]);
    u32 type_offset=friendly_sector_type(sector,size);
    // Wire battle type 2 decodes to native battle->type 1 in this client.
    // This non-ladder mode produces a real log without trophy/NPC counters.
    sector[type_offset]=2;
    sector_packet[0]=1;for(int i=0;i<4;++i)sector_packet[1+i]=(u8)(size>>(i*8));
    u32 compressed=sizeof sector_packet-5;
    if(CALL(symbol("compress2"),int,(void*,u32*,const void*,u32,int))(sector_packet+5,&compressed,sector,size,6)!=0)
        fail("GuestOffline: friendly sector compression failed");
    return compressed+5;
}
static void deliver_friendly_battle(void *self) {
    if(friendly_state!=2 || battle_started)fail("GuestOffline: invalid friendly battle delivery");
    u32 size=prepare_friendly_sector_packet();
    // Accepted state and both escrow debits are durable before sector delivery.
    write_save();reply(self,21903,1,sector_packet,size);battle_started=1;arena_battle=0;
    CALL(API_printf,int,(const char*,u32))("GuestOffline: local NPC accepted friendly challenge=%u; native battle started\n",friendly_sequence);
}
static void friendly_tick(void) {
    if(friendly_state!=1 || !logged_in || battle_started || wall_epoch()-friendly_stamp<2)return;
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    if(clan_status!=2 || pending_size)fail("GuestOffline: friendly acceptance during incomplete transaction");
    friendly_npc_after=0;friendly_state=2;friendly_accepted_stamp=wall_epoch();
    deliver_friendly_battle(active_messaging);
}
static void friendly_resume(void *self) {
    if(friendly_state==2 && !battle_started)deliver_friendly_battle(self);
}
static void friendly_close_entry(void) {
    struct GuestReader r={friendly_entry,friendly_entry_size,0,0,0};
    if(turn_vint(&r)!=10)fail("GuestOffline: friendly close is not a challenge entry");
    for(int i=0;i<6;++i)turn_vint(&r);clan_skip_string(&r,60,1);
    turn_vint(&r);turn_vint(&r);turn_vint(&r);turn_bool(&r);clan_skip_string(&r,512,0);
    if(r.at>=r.size || friendly_entry[r.at]&1)fail("GuestOffline: friendly close flag mismatch");
    friendly_entry[r.at]|=1; // Preserve the original immutable challenge text.
}
static void cancel_friendly(void *self,void *message) {
    if(CALL(0x198d19,u32,(void*))(message)!=0 || friendly_state!=1 || battle_started)
        fail("GuestOffline: friendly cancellation is only valid before local NPC acceptance");
    void *ui=CALL(0x91f7d,void*,(void))();capture_current_home(ui,1);
    void *logic=(void*)FIELD(ui,0x54);
    void *home=logic?(void*)FIELD(logic,0x30):0,*avatar=logic?(void*)FIELD(logic,0x34):0;
    if(!home || home!=delivered_home || home_clock_logic!=logic || !avatar ||
       CALL(0x9ab55,int,(void*))(avatar)!=friendly_gold_after ||
       CALL(0x9ab7d,int,(void*))(avatar)!=friendly_free_after)
        fail("GuestOffline: friendly cancellation wallet mismatch");
    void *gold_data=CALL(0xb8309,void*,(void))();
    void *free_data=CALL(0xb8319,void*,(void))();
    if(!gold_data || !free_data)fail("GuestOffline: friendly refund resource unavailable");
    CALL(0x9a5ed,int,(void*,int,void*,int,int))(avatar,0,gold_data,5,0);
    // Native Gold changes only clamp FreeGold downward. Restore its exact
    // pre-charge count through the original setter, not another Gold refund.
    CALL(0x9a845,void,(void*,int,void*,int))(avatar,0,free_data,friendly_free_before);
    if(CALL(0x9ab55,int,(void*))(avatar)!=friendly_gold_before ||
       CALL(0x9ab7d,int,(void*))(avatar)!=friendly_free_before)
        fail("GuestOffline: native friendly refund accounting mismatch");
    friendly_close_entry();friendly_state=4;capture_native(home,avatar);
    reply(self,24124,0,0,0);reply(self,24312,0,friendly_entry,friendly_entry_size);
    CALL(API_printf,int,(const char*,u32,int))("GuestOffline: cancelled friendly challenge=%u refund=5 gold=%d\n",friendly_sequence,friendly_gold_before);
}
static void abandon_friendly(void) {
    if(friendly_state!=2)fail("GuestOffline: invalid friendly abandonment");
    friendly_close_entry();friendly_state=5;write_save();
    reply(active_messaging,24312,0,friendly_entry,friendly_entry_size);
    CALL(API_puts,int,(const char*))("GuestOffline: friendly battle abandoned without result or refund");
}
static void friendly_battle_completed(void *battle) {
    if(friendly_state!=2 || !battle_started || !ending_battle)fail("GuestOffline: unexpected friendly battle completion");
    void *mode=(void*)FIELD(battle,0),*live=CALL(0x93c75,void*,(void*))(mode);
    void *owner=decode_packet(24101,0,saved_home,saved_size),*saved=(void*)FIELD(owner,0x34);
    if(!live || !saved || FIELD(live,0x34)!=FIELD(saved,0x34) || FIELD(live,0x9c)!=FIELD(saved,0x9c) ||
       FIELD(live,0xa0)!=FIELD(saved,0xa0) || CALL(0x9b081,int,(void*))(live)!=CALL(0x9b081,int,(void*))(saved) ||
       CALL(0x9ab55,int,(void*))(live)!=friendly_gold_after)
        fail("GuestOffline: friendly battle mutated ladder wallet/counters/crowns");
    void *log=(void*)FIELD(battle,0xb0);if(!log)fail("GuestOffline: friendly result has no native battle log");
    void *json=CALL(0x1c905,void*,(void*))(log);u32 builder[4]={0},text[5]={0};
    CALL(0x18f089,void,(void*))(builder);CALL(0x1962f5,void,(void*,void*))(json,builder);
    CALL(0x18ec05,void,(void*,void*))(text,builder);
    u32 length=CALL(0x18f4bd,u32,(void*))(text);const u8 *bytes=CALL(0x19c099,const u8*,(void*))(text);
    if(!length || length>30000 || !bytes)fail("GuestOffline: invalid native friendly battle JSON");
    u32 stream[16],entry[16]={0};CALL(0x192a71,void,(void*,u32))(stream,65536);
    CALL(0x193749,void,(void*,int))(stream,11);friendly_stream_base(entry,saved);
    FIELD(entry,0x24)=(u32)text;FIELD(entry,0x30)=2;FIELD(entry,0x34)=0;
    FIELD(entry,0x38)=1306;FIELD(entry,0x3c)=0;
    CALL(0x1b78d,void,(void*,void*))(entry,stream);
    friendly_result_size=CALL(0x192b2d,u32,(void*))(stream);
    if(friendly_result_size>sizeof friendly_result)fail("GuestOffline: friendly result exceeds bound");
    bytes=CALL(0x1936fd,const u8*,(void*))(stream);
    for(u32 i=0;i<friendly_result_size;++i)friendly_result[i]=bytes[i];
    friendly_state=3;write_save();reply(active_messaging,24312,0,friendly_result,friendly_result_size);
    CALL(0x19370d,void,(void*))(stream);CALL(0x18db2d,void,(void*))(text);CALL(0x18ec15,void,(void*))(builder);
    u32 *vtable=(u32*)FIELD(json,0);CALL(vtable[1],void,(void*))(json);dispose(owner);
    CALL(API_printf,int,(const char*,u32,u32))("GuestOffline: actual friendly result=%u bytes=%u; no ladder rewards\n",friendly_sequence,friendly_result_size);
}
