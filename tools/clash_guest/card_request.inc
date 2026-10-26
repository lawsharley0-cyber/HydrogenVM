/* Original command 524 creates a real, unfulfilled request. */
extern int guest_original_request_execute(void*,void*,int,int);
int guest_request_execute(void *command,void *logic,int mode,int flags) {
    if((mode&2) && save_initialized && !pending_size && clan_status==2 && home_clock_logic==logic) {
        void *home=CALL(0xd7355,void*,(void*))(logic);
        if(home==delivered_home)account_home_clock(logic,home,1);
    }
    int result=guest_original_request_execute(command,logic,mode,flags);
    if(result || !(mode&2))return result;
    void *home=CALL(0xd7355,void*,(void*))(logic),*avatar=CALL(0xd742d,void*,(void*))(logic);
    if(!save_initialized || !avatar || FIELD(avatar,0x10)!=0 || FIELD(avatar,0x14)!=1)return result;
    if(!home || home!=delivered_home || home_clock_logic!=logic || clan_status!=2 || pending_size ||
       card_request_sequence>=1000000000 || CALL(0x126971,int,(void*))(home)<=0 || FIELD(command,0x24)!=1)
        fail("GuestOffline: native card request completed outside a valid local clan transaction");
    u32 previous=card_request_size?card_request_sequence:0;
    u32 stream[16],entry[13]={0},identity[2]={1,++card_request_sequence},empty_donors[3]={0};
    CALL(0x192a71,void,(void*,u32))(stream,2048);
    CALL(0x192d0d,void,(void*,void*))(stream,(void*)FIELD(command,0x18));
    CALL(0xdc639,void,(void*,void*))(stream,(u8*)command+0x1c);
    card_request_echo_crc=checksum(CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    CALL(0x19370d,void,(void*))(stream);
    CALL(0x192a71,void,(void*,u32))(stream,2048);CALL(0x193749,void,(void*,int))(stream,1);
    FIELD(entry,4)=(u32)identity;FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(avatar);
    FIELD(entry,0xc)=FIELD(entry,8);FIELD(entry,0x10)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(avatar);FIELD(entry,0x18)=2;
    void *arena=CALL(0x9b4d1,void*,(void*))(avatar);
    FIELD(entry,0x24)=(u32)CALL(0x123ef1,int,(void*))(arena);
    FIELD(entry,0x28)=(u32)empty_donors;FIELD(entry,0x2c)=(u32)((u8*)command+0x1c);
    FIELD(entry,0x30)=FIELD(command,0x18);
    CALL(0x11051d,void,(void*,void*))(entry,stream);
    const u8 *data=CALL(0x1936fd,const u8*,(void*))(stream);u32 now=wall_epoch();
    struct SectorWriter checked={clan_card_request,0,sizeof clan_card_request};
    card_request_entry(&checked,data,CALL(0x192b2d,u32,(void*))(stream),now,now);
    card_request_size=checked.size;card_request_stamp=now;
    account_home_clock(logic,home,0);capture_native(home,avatar);
    if(!logged_in || !active_messaging || !BYTE(active_messaging,0xe4))
        fail("GuestOffline: local card request conversation unavailable");
    if(previous) {
        // Original AllianceStreamEntryRemovedMessage, type getter 0x1aeac,
        // encodes one fixed BE LogicLong. Match the durable replacement live.
        u8 removed[8];struct SectorWriter w={removed,0,sizeof removed};
        clan_int(&w,1);clan_int(&w,previous);reply(active_messaging,24318,0,removed,w.size);
    }
    reply(active_messaging,24312,0,clan_card_request,card_request_size);
    CALL(0x19370d,void,(void*))(stream);
    CALL(API_puts,int,(const char*))("GuestOffline: original card request and cooldown committed; no donations granted");
    return result;
}
static void request_echo(struct GuestReader *r) {
    u32 start=r->at;clan_skip_string(r,1024,0);
    if(turn_vint(r)!=1)fail("GuestOffline: invalid echoed card request wishlist");
    int type=turn_vint(r),index=turn_vint(r);
    if((type!=26 && type!=28) || index<0 || index>=200 || !card_request_size ||
       checksum(r->data+start,r->at-start)!=card_request_echo_crc)
        fail("GuestOffline: card request echo differs from completed native command");
}
