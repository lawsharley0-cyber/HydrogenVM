static void validate_community(void) {
    if(!community_clan)return;
    if(community_clan!=clan_serial || clan_status<2 || clan_status>4 || !donor_size || donor_inventory_count!=6 ||
       (community_loaded && community_binding!=checksum(donor_home,donor_size)))
        fail("GuestOffline: clan community has no matching native actors");
    u8 checked[256];struct SectorWriter w={checked,0,sizeof checked};
    community_entry_wire(&w,community_entry,community_size,community_stamp,community_stamp);
    community_loaded=0;
}
static void community_initialize(void) {
    if(community_clan)return;
    if(clan_status!=2 || !donor_size || donor_inventory_count!=6)fail("GuestOffline: no NPC for clan community");
    community_clan=clan_serial;community_week=community_week_start(wall_epoch());
    if(!community_week)fail("GuestOffline: invalid community wall clock");
    // Legacy weekly event times are unavailable. Start at zero, without
    // retroactively assigning lifetime donations to a fabricated week.
    community_human_sent=0;community_npc_sent=0;community_human_received=0;community_npc_received=0;
    void *packet=decode_packet(24101,0,donor_home,donor_size),*avatar=(void*)FIELD(packet,0x34);
    u32 entry[10]={0},identity[2]={4,1},text[5],stream[16];
    CALL(0x18dcf9,void,(void*,const char*))(text,community_message);
    CALL(0x192a71,void,(void*,u32))(stream,sizeof community_entry);CALL(0x193749,void,(void*,int))(stream,2);
    FIELD(entry,4)=(u32)identity;FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(avatar);FIELD(entry,0xc)=FIELD(entry,8);
    FIELD(entry,0x10)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(avatar);FIELD(entry,0x18)=(u32)CALL(0x9ae41,int,(void*))(avatar);
    FIELD(entry,0x24)=(u32)text;CALL(0x11ca71,void,(void*,void*))(entry,stream);
    community_stamp=wall_epoch();struct SectorWriter w={community_entry,0,sizeof community_entry};
    community_entry_wire(&w,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream),community_stamp,community_stamp);
    community_size=w.size;CALL(0x19370d,void,(void*))(stream);CALL(0x18db2d,void,(void*))(text);dispose(packet);
    CALL(API_puts,int,(const char*))("GuestOffline: local NPC profile message and weekly tracking initialized; no retroactive grants");
}
static int community_rollover(u32 now) {
    u32 week=community_week_start(now);
    if(!community_clan || week<=community_week)return 0;
    community_week=week;community_human_sent=0;community_npc_sent=0;
    community_human_received=0;community_npc_received=0;return 1;
}
static void community_points(int human,int capacity) {
    if(!community_clan || (capacity!=1 && capacity!=10))fail("GuestOffline: missing weekly donation context");
    community_rollover(wall_epoch());
    u32 *sent=human?&community_human_sent:&community_npc_sent;
    u32 *received=human?&community_npc_received:&community_human_received;
    if(*sent>1000000000u-(u32)capacity || *received!=*sent)fail("GuestOffline: weekly donation capacity overflow/mismatch");
    *sent+=(u32)capacity;*received+=(u32)capacity;
}
static void community_tick(void) {
    static u32 checked_epoch;
    if(!logged_in || battle_started || clan_status!=2 || !community_clan || pending_size ||
       initial_name_pending || friendly_state==1 || friendly_state==2)return;
    u32 now=wall_epoch();if(checked_epoch && now>=checked_epoch && now-checked_epoch<60)return;
    checked_epoch=now;if(community_rollover(now))write_save();
}
static void invite_local_npc(void *self,void *message) {
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader r={data,size,0,0,0};
    if(turn_int(&r)!=3 || turn_int(&r)!=1 || r.at!=r.size || clan_status!=2 || !donor_size ||
       pending_size || initial_name_pending || friendly_state==1 || friendly_state==2)
        fail("GuestOffline: invalid local NPC invitation");
    void *ui=CALL(0x91f7d,void*,(void))();void *logic=ui && !FIELD(ui,0x28)?(void*)FIELD(ui,0x54):0;
    void *home=logic?(void*)FIELD(logic,0x30):0,*avatar=logic?(void*)FIELD(logic,0x34):0;
    if(!home || !avatar || home!=delivered_home || logic!=home_clock_logic || BYTE(home,0xb4) ||
       !BYTE(CALL(0x9ae45,void*,(void*))(avatar),0x4c))
        fail("GuestOffline: native invitation permission/home check failed");
    // A resent response after a committed NPC acceptance does not rejoin or
    // recreate it. Only this actual NPC/current clan can be acknowledged.
    if(!donor_present) {
        capture_current_home(ui,1);
        struct NativeActor npc=native_actor(donor_home,donor_size);
        if(CALL(0x9ae01,void*,(void*))(npc.avatar))fail("GuestOffline: invited NPC already belongs to a clan");
        u32 now=wall_epoch(),elapsed=guest_elapsed_home_seconds(donor_epoch,now);
        if(elapsed)CALL(0x125d7d,void,(void*,int,int))(npc.home,(int)elapsed,-1);
        void *request=decode_packet(14301,0,clan_request,clan_request_size);
        u32 command[11]={0},id[2]={1,clan_serial};CALL(0xe5781,void,(void*))(command);
        FIELD(command,0x1c)=(u32)id;FIELD(command,0x20)=FIELD(request,0x38);FIELD(command,0x24)=FIELD(request,0x30);
        BYTE(command,0x28)=0;int result=CALL(0xe5875,int,(void*,void*,int,int))(command,npc.logic,2,0);
        FIELD(command,0x1c)=0;FIELD(command,0x20)=0;FIELD(command,0x24)=0;CALL(0xe57a9,void,(void*))(command);
        dispose(request);void *joined=CALL(0x9ae01,void*,(void*))(npc.avatar);
        if(result || !joined || FIELD(joined,0)!=1 || FIELD(joined,4)!=clan_serial ||
           CALL(0x9ae41,int,(void*))(npc.avatar)!=1)fail("GuestOffline: original NPC invitation acceptance/join failed");
        donor_size=native_actor_encode(npc.home,npc.avatar,donor_home,sizeof donor_home,&donor_offset);
        if(now>donor_epoch)donor_epoch=now;
        donor_present=1;donor_role=1;native_actor_destroy(npc);validate_donor_inventory();
        account_home_clock(logic,home,0);capture_native(home,avatar);
        CALL(API_puts,int,(const char*))("GuestOffline: local NPC accepted native invitation; retained actor/inventory/cooldowns");
    }
    reply(self,24322,0,0,0); // Original AllianceInvitationSentOkMessage, empty body.
    u8 query[8];struct SectorWriter w={query,0,sizeof query};clan_id(&w);
    void *request=decode_packet(14302,0,query,w.size);clan_data(self,request);dispose(request);
}
