/* Original 14306/14307 -> 24306/24307 controls over the actual local NPC.
 * Native210 changes role; native208 removes membership. Both avatars and the
 * durable kick tombstone share one atomic commit before success delivery. */
static void native_clan_role(void *logic,int role) {
    u32 command[9]={0},id[2]={1,clan_serial};CALL(0x417bd,void,(void*))(command);
    FIELD(command,0x1c)=(u32)id;FIELD(command,0x20)=(u32)role;
    int result=CALL(0x41801,int,(void*,void*,int,int))(command,logic,2,0);
    FIELD(command,0x1c)=0;CALL(0x417dd,void,(void*))(command);
    void *avatar=CALL(0xd742d,void*,(void*))(logic);
    if(result || !avatar || CALL(0x9ae41,int,(void*))(avatar)!=role)
        fail("GuestOffline: original clan role command did not apply");
}
static void change_local_member(void *self,void *message,int kick) {
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader r={data,size,0,0,0};
    if(turn_int(&r)!=3 || turn_int(&r)!=1)fail("GuestOffline: unknown local member control target");
    int role=0;
    if(kick)clan_skip_string(&r,1024,0);else role=turn_vint(&r);
    if(r.at!=r.size || (!kick && (role<1 || role>4)))fail("GuestOffline: invalid member control payload");
    if(!donor_size || clan_status!=2 || pending_size || initial_name_pending ||
       friendly_state==1 || friendly_state==2)fail("GuestOffline: member control during another transaction");
    void *ui=CALL(0x91f7d,void*,(void))();
    if(!ui || FIELD(ui,0x28)!=0)fail("GuestOffline: member control outside the native home screen");
    void *logic=(void*)FIELD(ui,0x54);
    void *home=logic?(void*)FIELD(logic,0x30):0,*avatar=logic?(void*)FIELD(logic,0x34):0;
    if(!home || !avatar || home!=delivered_home || home_clock_logic!=logic || BYTE(home,0xb4))
        fail("GuestOffline: member control has no current native home");
    int own=CALL(0x9ae41,int,(void*))(avatar);
    if(!clan_roles_valid(own,(int)donor_present,donor_role))fail("GuestOffline: invalid local leader topology");
    struct NativeActor npc=native_actor(donor_home,donor_size);
    int old=CALL(0x9ae41,int,(void*))(npc.avatar);
    // An exact already committed operation can be acknowledged again without
    // touching either actor. This includes a response lost after the rename.
    if((kick && !donor_present) || (!kick && donor_present && role==old)) {
        native_actor_destroy(npc);reply(self,kick?24307:24306,0,data,kick?8:size);return;
    }
    if(!donor_present || (kick?
       (!BYTE(CALL(0x9ae45,void*,(void*))(avatar),0x50) || !CALL(0x38025,int,(int,int))(old,own)):
       (!CALL(0xa9795,int,(int,int,int))(own,old,role) || (role==2 && old!=4))))
        fail("GuestOffline: native clan rank/permission denies member operation");
    capture_current_home(ui,1);
    u32 now=wall_epoch(),elapsed=guest_elapsed_home_seconds(donor_epoch,now);
    if(elapsed)CALL(0x125d7d,void,(void*,int,int))(npc.home,(int)elapsed,-1);
    if(kick) {
        u32 command[9]={0},id[2]={1,clan_serial};CALL(0x1250d,void,(void*))(command);
        FIELD(command,0x1c)=(u32)id;BYTE(command,0x20)=1;
        int result=CALL(0x1254d,int,(void*,void*,int,int))(command,npc.logic,2,0);
        FIELD(command,0x1c)=0;CALL(0x1252d,void,(void*))(command);
        if(result || CALL(0x9ae01,void*,(void*))(npc.avatar))fail("GuestOffline: original NPC kick/leave failed");
        donor_present=0;npc_request_size=0;npc_request_stamp=0;
    } else {
        native_clan_role(npc.logic,role);
        if(role==2)native_clan_role(logic,4); // Original handoff: previous leader becomes co-leader.
    }
    donor_role=CALL(0x9ae41,int,(void*))(npc.avatar);
    if(!clan_roles_valid(CALL(0x9ae41,int,(void*))(avatar),(int)donor_present,donor_role))
        fail("GuestOffline: member operation did not leave exactly one leader");
    donor_size=native_actor_encode(npc.home,npc.avatar,donor_home,sizeof donor_home,&donor_offset);
    if(now>donor_epoch)donor_epoch=now;
    native_actor_destroy(npc);account_home_clock(logic,home,0);capture_native(home,avatar);
    reply(self,kick?24307:24306,0,data,kick?8:size);
    if(kick && npc_request_sequence) {
        u8 removed[8];struct SectorWriter w={removed,0,sizeof removed};
        clan_int(&w,3);clan_int(&w,npc_request_sequence);reply(self,24318,0,removed,w.size);
    }
    CALL(API_printf,int,(const char*,int,int,int))("GuestOffline: atomic member control kick=%d role=%d->%d\n",kick,old,kick?0:role);
}
#include "clan_member_wire.inc"
static void kick_from_turn(const u8 *data,u32 size) {
    void *message=decode_packet(14307,0,data,size);
    change_local_member(active_messaging,message,1);dispose(message);
}
