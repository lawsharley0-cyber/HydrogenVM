/* One truthful local NPC, original524 requests and original203/211 transfers.
 * The ordinary native UI emits14310; no predicted/artificial stock or reward. */
static void validate_npc_request(void) {
    if(!npc_request_sequence)return;
    if(!donor_size || donor_inventory_count!=6 ||
       (npc_request_loaded && npc_request_binding!=checksum(donor_home,donor_size)) ||
       (npc_request_size && !donor_present))fail("GuestOffline: NPC request has no matching actor");
    if(npc_request_size) {
        u8 checked[2048];struct SectorWriter w={checked,0,sizeof checked};
        struct CardRequestInfo r=npc_request_entry(&w,npc_request,npc_request_size,npc_request_stamp,npc_request_stamp,npc_request_sequence);
        struct DonorInventoryRow *row=donor_inventory_row((u32)(r.type*1000000+r.index));
        if(row->received<r.card_count)fail("GuestOffline: NPC request receipt differs from inventory history");
    }
    npc_request_loaded=0;
}
static void npc_request_remove(void *self,u32 sequence) {
    u8 removed[8];struct SectorWriter w={removed,0,sizeof removed};
    clan_int(&w,3);clan_int(&w,sequence);reply(self,24318,0,removed,w.size);
}
static void npc_request_tick(void) {
    static u32 checked_epoch;
    if(!logged_in || !active_messaging || !BYTE(active_messaging,0xe4) || battle_started ||
       clan_status!=2 || !donor_present || !donor_size || pending_size || initial_name_pending ||
       friendly_state==1 || friendly_state==2 || npc_request_sequence>=1000000000u)return;
    u32 now=wall_epoch();
    if(checked_epoch && now>=checked_epoch && now-checked_epoch<60)return;
    void *ui=CALL(0x91f7d,void*,(void))();if(!ui || FIELD(ui,0x28))return;
    void *logic=(void*)FIELD(ui,0x54),*home=logic?(void*)FIELD(logic,0x30):0;
    if(!home || home!=delivered_home || logic!=home_clock_logic || BYTE(home,0xb4))return;
    checked_epoch=now;
    struct NativeActor npc=native_actor(donor_home,donor_size);
    u32 elapsed=guest_elapsed_home_seconds(donor_epoch,now);
    if(elapsed)CALL(0x125d7d,void,(void*,int,int))(npc.home,(int)elapsed,-1);
    if(CALL(0x126971,int,(void*))(npc.home)>0) {native_actor_destroy(npc);return;}
    u32 command[10]={0},start=0;int result=-1;
    // Exact remainder without an unavailable ARM compiler-runtime divide import.
    for(int bit=31;bit>=0;--bit) {start=(start<<1)|((npc_request_sequence>>bit)&1u);if(start>=6)start-=6;}
    for(u32 i=0;i<6;++i) {
        u32 index=start+i;if(index>=6)index-=6;
        void *stock=donation_find_card(npc.home,(int)donor_starter_cards[index]);
        void *data=CALL(0x3755d,void*,(void*))(stock);
        CALL(0x20525,void,(void*))(command);
        void *message=CALL(API_malloc,void*,(u32))(20);if(!message)fail("GuestOffline: NPC request allocation failed");
        CALL(0x18dcf9,void,(void*,const char*))(message,"Can you spare a card?");
        CALL(0x209f5,void,(void*,void*))(command,message);CALL(0x209f9,void,(void*,void*))(command,data);
        result=guest_original_request_execute(command,npc.logic,2,0);
        if(!result)break;
        CALL(0x20549,void,(void*))(command);
    }
    if(result) {native_actor_destroy(npc);return;}
    if(CALL(0x126971,int,(void*))(npc.home)<=0 || FIELD(command,0x24)!=1)
        fail("GuestOffline: native NPC request has no cooldown/wishlist");
    capture_current_home(ui,1);
    u32 previous=npc_request_size?npc_request_sequence:0,stream[16],entry[13]={0};
    u32 identity[2]={3,++npc_request_sequence},empty_donors[3]={0};
    CALL(0x192a71,void,(void*,u32))(stream,2048);CALL(0x193749,void,(void*,int))(stream,1);
    FIELD(entry,4)=(u32)identity;FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(npc.avatar);
    FIELD(entry,0xc)=FIELD(entry,8);FIELD(entry,0x10)=(u32)CALL(0x99e45,void*,(void*))(npc.avatar);
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(npc.avatar);
    FIELD(entry,0x18)=(u32)CALL(0x9ae41,int,(void*))(npc.avatar);
    FIELD(entry,0x24)=(u32)CALL(0x123ef1,int,(void*))(CALL(0x9b4d1,void*,(void*))(npc.avatar));
    FIELD(entry,0x28)=(u32)empty_donors;FIELD(entry,0x2c)=(u32)((u8*)command+0x1c);FIELD(entry,0x30)=FIELD(command,0x18);
    CALL(0x11051d,void,(void*,void*))(entry,stream);
    struct SectorWriter w={npc_request,0,sizeof npc_request};
    npc_request_entry(&w,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream),now,now,npc_request_sequence);
    npc_request_size=w.size;npc_request_stamp=now;
    donor_size=native_actor_encode(npc.home,npc.avatar,donor_home,sizeof donor_home,&donor_offset);
    if(now>donor_epoch)donor_epoch=now;
    CALL(0x19370d,void,(void*))(stream);CALL(0x20549,void,(void*))(command);native_actor_destroy(npc);
    account_home_clock(logic,home,0);capture_native(home,(void*)FIELD(logic,0x34));
    // Seed a full ordered conversation before sending deltas. A pre-snapshot
    // delta otherwise leaves the native cache's newest request above old posts.
    if(clan_stream_ready) {
        if(previous)npc_request_remove(active_messaging,previous);
        reply(active_messaging,24312,0,npc_request,npc_request_size);
    }
    CALL(API_puts,int,(const char*))("GuestOffline: native NPC request and cooldown committed");
}
static void donate_to_npc(void *self,void *message) {
    const u8 *bytes=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader r={bytes,size,0,0,0};int type=turn_vint(&r),index=turn_vint(&r);
    u32 space=turn_int(&r),sequence=turn_int(&r);
    if(r.at!=r.size || space!=3 || !sequence || sequence>npc_request_sequence ||
       (type!=26 && type!=28) || index<0 || index>=200)fail("GuestOffline: invalid NPC donation target");
    if(!npc_request_size || sequence!=npc_request_sequence || !donor_present) {
        npc_request_remove(self,sequence);return;
    }
    if(clan_status!=2 || pending_size || initial_name_pending || friendly_state==1 || friendly_state==2)
        fail("GuestOffline: NPC donation during another transaction");
    u8 checked[2048];struct SectorWriter w={checked,0,sizeof checked};
    struct CardRequestInfo request=npc_request_entry(&w,npc_request,npc_request_size,npc_request_stamp,npc_request_stamp,npc_request_sequence);
    if(type!=request.type || index!=request.index)fail("GuestOffline: donated card differs from NPC request");
    u32 now=wall_epoch();
    if(now>npc_request_stamp && now-npc_request_stamp>=28800) {npc_request_remove(self,sequence);return;}
    void *ui=CALL(0x91f7d,void*,(void))();
    void *logic=ui && !FIELD(ui,0x28)?(void*)FIELD(ui,0x54):0;
    void *home=logic?(void*)FIELD(logic,0x30):0,*avatar=logic?(void*)FIELD(logic,0x34):0;
    if(!home || !avatar || home!=delivered_home || logic!=home_clock_logic || BYTE(home,0xb4))
        fail("GuestOffline: NPC donation outside native home");
    void *stock=donation_find_card(home,type*1000000+index);
    if(!stock || !FIELD(stock,0xc)) {reply(self,24312,0,npc_request,npc_request_size);return;}
    void *data=CALL(0x3755d,void*,(void*))(stock),*id=CALL(0x99e2d,void*,(void*))(avatar);
    // Original DonateStreamEntry checks weighted capacity and per-player/arena
    // limits. Decode our exact entry; do not replace these rules with a no-op.
    void *packet=decode_packet(24312,0,npc_request,npc_request_size);
    int allowed=CALL(0x1108fd,int,(void*,void*,void*,int,void*))((void*)FIELD(packet,0x30),id,data,1,
        CALL(0x9b4d1,void*,(void*))(avatar));dispose(packet);
    if(!allowed) {reply(self,24312,0,npc_request,npc_request_size);return;}
    struct NativeActor npc=native_actor(donor_home,donor_size);
    u32 elapsed=guest_elapsed_home_seconds(donor_epoch,now);
    if(elapsed)CALL(0x125d7d,void,(void*,int,int))(npc.home,(int)elapsed,-1);
    void *received=donation_find_card(npc.home,type*1000000+index);
    struct DonorInventoryRow *row=donor_inventory_row((u32)(type*1000000+index));
    u32 expected,before=FIELD(stock,0xc),old=FIELD(received,0xc);
    if(!donor_inventory_stock(row,&expected) || expected!=old || old>=1000000000u || row->received>=1000000000u)
        fail("GuestOffline: NPC receive inventory bound/provenance mismatch");
    capture_current_home(ui,1);
    int total=CALL(0x9b861,int,(void*))(avatar);
    u32 send[9]={0};CALL(0x10399,void,(void*))(send);
    u32 *target=CALL(API_malloc,void*,(u32))(8);if(!target)fail("GuestOffline: donation target allocation failed");
    target[0]=3;target[1]=1;CALL(0x103dd,void,(void*,void*,void*))(send,target,data);
    int result=CALL(0x10439,int,(void*,void*,int,int))(send,logic,2,0);CALL(0x103b9,void,(void*))(send);
    if(result || FIELD(stock,0xc)==before) {native_actor_destroy(npc);reply(self,24312,0,npc_request,npc_request_size);return;}
    if(FIELD(stock,0xc)!=before-1 || CALL(0x9b861,int,(void*))(avatar)!=total+1)
        fail("GuestOffline: native player donation delta mismatch");
    u32 receive[9]={0};CALL(0x21be9,void,(void*))(receive);
    void *name=CALL(API_malloc,void*,(u32))(20);if(!name)fail("GuestOffline: receipt allocation failed");
    CALL(0x18dcf9,void,(void*,const char*))(name,CALL(0x19c099,const char*,(void*))(CALL(0x99e45,void*,(void*))(avatar)));
    FIELD(receive,0x1c)=(u32)name;FIELD(receive,0x20)=(u32)data;
    result=CALL(0x21d1d,int,(void*,void*,int,int))(receive,npc.logic,2,0);CALL(0x21c09,void,(void*))(receive);
    if(result || FIELD(received,0xc)!=old+1)fail("GuestOffline: native NPC receipt delta mismatch");
    ++row->received;
    u32 stream[16],donor_id[2]={0,1};void *cards[10];u32 count=request.card_count+1;
    if(count>10)fail("GuestOffline: NPC donor list bound exceeded");
    for(u32 i=0;i<count;++i)cards[i]=data;
    u32 list[3]={(u32)cards,count,count},container[2]={(u32)list,(u32)donor_id};
    CALL(0x192a71,void,(void*,u32))(stream,512);CALL(0xa944d,void,(void*,void*))(container,stream);
    w=(struct SectorWriter){checked,0,sizeof checked};sector_bytes(&w,npc_request,request.donors_at);clan_int(&w,1);
    sector_bytes(&w,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    sector_bytes(&w,npc_request+request.donors_end,npc_request_size-request.donors_end);CALL(0x19370d,void,(void*))(stream);
    npc_request_size=w.size;for(u32 i=0;i<w.size;++i)npc_request[i]=checked[i];
    donor_size=native_actor_encode(npc.home,npc.avatar,donor_home,sizeof donor_home,&donor_offset);
    if(now>donor_epoch)donor_epoch=now;
    native_actor_destroy(npc);account_home_clock(logic,home,0);capture_native(home,avatar);
    reply(self,24312,0,npc_request,npc_request_size);
    CALL(API_printf,int,(const char*,u32,u32,u32))("GuestOffline: player donated to NPC stock=%u->%u received=%u\n",before,before-1,count);
}
