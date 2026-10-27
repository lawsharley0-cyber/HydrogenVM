/* All native mutations run synchronously on the guest home/update thread.
 * Neither command enters a deferred server queue. The old atomic checkpoint
 * remains authoritative until BOTH actors and the request are committed in one
 * rename. An interrupted pre-rename transfer rolls back both; post-rename
 * request donors prevent another execution. No split-sidecar grant journal. */
static void *donation_find_card(void *home,int global_id) {
    void *collection=CALL(0x125609,void*,(void*))(home),*deck=CALL(0x1255fd,void*,(void*))(home);
    int deck_count=CALL(0x16c55,int,(void*))(deck),collection_count=(int)FIELD(collection,8);
    if(deck_count<0 || deck_count>8 || collection_count<0 || collection_count>200)
        fail("GuestOffline: local donor inventory extent invalid");
    for(int group=0;group<2;++group) {
        int count=group?collection_count:deck_count;
        for(int i=0;i<count;++i) {
            void *card=group?CALL(0x83571,void*,(void*,int))(collection,i):CALL(0x16b15,void*,(void*,int))(deck,i);
            if(CALL(0x7d2e1,int,(void*))(CALL(0x3755d,void*,(void*))(card))==global_id)return card;
        }
    }
    return 0;
}
#include "donor_inventory.inc"
static void validate_donation_record(void) {
    if(!card_request_size)return;
    static u8 checked[2048];struct SectorWriter w={checked,0,sizeof checked};
    struct CardRequestInfo request=card_request_entry(&w,clan_card_request,card_request_size,card_request_stamp,card_request_stamp);
    if(!request.donors)return;
    if(!donor_size)fail("GuestOffline: fulfilled request has no persistent donor");
    struct DonorInventoryRow *row=donor_inventory_row((u32)(request.type*1000000+request.index));
    if(!row->sent)
        fail("GuestOffline: fulfilled request differs from finite donor inventory/history");
}
static void donation_tick(void) {
    static u32 checked_sequence,checked_epoch;
    if(!logged_in || !active_messaging || !BYTE(active_messaging,0xe4) || battle_started ||
       clan_status!=2 || !donor_present || !donor_size || !card_request_size || pending_size || initial_name_pending ||
       friendly_state==1 || friendly_state==2)return;
    u32 now=wall_epoch();
    if(now<card_request_stamp || now-card_request_stamp<5 || now-card_request_stamp>=28800)return;
    if(checked_sequence==card_request_sequence && now>=checked_epoch && now-checked_epoch<60)return;
    void *ui=CALL(0x91f7d,void*,(void))();
    if(!ui || FIELD(ui,0x28)!=0)return;
    void *logic=(void*)FIELD(ui,0x54);
    void *home=logic?(void*)FIELD(logic,0x30):0,*avatar=logic?(void*)FIELD(logic,0x34):0;
    if(!home || !avatar || home!=delivered_home || home_clock_logic!=logic || BYTE(home,0xb4))return;
    checked_sequence=card_request_sequence;checked_epoch=now;
    static u8 checked[2048];struct SectorWriter w={checked,0,sizeof checked};
    struct CardRequestInfo request=card_request_entry(&w,clan_card_request,card_request_size,card_request_stamp,card_request_stamp);
    if(request.donors)return;
    capture_current_home(ui,1);
    struct NativeActor donor=native_actor(donor_home,donor_size);
    u32 elapsed=guest_elapsed_home_seconds(donor_epoch,now);
    if(elapsed)CALL(0x125d7d,void,(void*,int,int))(donor.home,(int)elapsed,-1);
    // Resolve by the actual owned collection instead of fabricating a card.
    void *stock=donation_find_card(donor.home,request.type*1000000+request.index);
    if(!stock || (int)FIELD(stock,0xc)<=0 || CALL(0x126a59,int,(void*))(donor.home)>0) {
        native_actor_destroy(donor);return;
    }
    void *data=CALL(0x3755d,void*,(void*))(stock);
    int capacity=CALL(0x755f1,int,(void*))(data);
    void *received=CALL(0x1262c1,void*,(void*,void*))(home,data);
    void *id=CALL(0x9ae01,void*,(void*))(avatar);
    void *arena=CALL(0x9b4d1,void*,(void*))(avatar);
    if(!id || FIELD(id,0)!=1 || FIELD(id,4)!=donor_clan || !received ||
       (capacity!=1 && capacity!=10) || request.capacity!=(u32)CALL(0x123ef1,int,(void*))(arena) ||
       CALL(0x123ef5,int,(void*))(arena)<1 || FIELD(received,0xc)>=1000000000u) {
        native_actor_destroy(donor);return;
    }
    int before_stock=(int)FIELD(stock,0xc),before_received=(int)FIELD(received,0xc);
    int gold=CALL(0x9ab55,int,(void*))(donor.avatar),xp=CALL(0x9abc5,int,(void*))(donor.avatar);
    int human_gold=CALL(0x9ab55,int,(void*))(avatar),human_xp=CALL(0x9abc5,int,(void*))(avatar);
    u32 send[9]={0};CALL(0x10399,void,(void*))(send);
    u32 *target=CALL(API_malloc,void*,(u32))(8);if(!target)fail("GuestOffline: no donation target allocation");
    target[0]=0;target[1]=1;CALL(0x103dd,void,(void*,void*,void*))(send,target,data);
    int result=CALL(0x10439,int,(void*,void*,int,int))(send,donor.logic,2,0);
    CALL(0x103b9,void,(void*))(send);
    if(result || (int)FIELD(stock,0xc)==before_stock) {
        // A zero result can be a native listener failure. Discard this detached
        // actor; no receiver command, persistent donor change or reward follows.
        native_actor_destroy(donor);return;
    }
    if((int)FIELD(stock,0xc)!=before_stock-1 ||
       CALL(0x9ab55,int,(void*))(donor.avatar)-gold!=CALL(0x75649,int,(void*))(data) ||
       CALL(0x9abc5,int,(void*))(donor.avatar)-xp!=CALL(0x75651,int,(void*))(data))
        fail("GuestOffline: native donor inventory/reward delta mismatch");
    u32 receive[9]={0};CALL(0x21be9,void,(void*))(receive);
    void *name=CALL(API_malloc,void*,(u32))(20);if(!name)fail("GuestOffline: no donation donor name allocation");
    CALL(0x18dcf9,void,(void*,const char*))(name,"Local Donor");
    FIELD(receive,0x1c)=(u32)name;FIELD(receive,0x20)=(u32)data;
    result=CALL(0x21d1d,int,(void*,void*,int,int))(receive,logic,2,0);CALL(0x21c09,void,(void*))(receive);
    if(result || (int)FIELD(received,0xc)!=before_received+1 ||
       CALL(0x9ab55,int,(void*))(avatar)!=human_gold || CALL(0x9abc5,int,(void*))(avatar)!=human_xp)
        fail("GuestOffline: native receiver inventory/wallet delta mismatch");
    donor_inventory_sent((u32)(request.type*1000000+request.index),(u32)before_stock);
    u32 stream[16],donor_id[2]={3,1};void *cards[1]={data};
    u32 list[3]={(u32)cards,1,1},container[2]={(u32)list,(u32)donor_id};
    CALL(0x192a71,void,(void*,u32))(stream,256);CALL(0xa944d,void,(void*,void*))(container,stream);
    w=(struct SectorWriter){checked,0,sizeof checked};sector_bytes(&w,clan_card_request,request.donors_at);
    clan_int(&w,1);sector_bytes(&w,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    sector_bytes(&w,clan_card_request+request.donors_at+4,card_request_size-request.donors_at-4);
    CALL(0x19370d,void,(void*))(stream);
    card_request_size=w.size;for(u32 i=0;i<w.size;++i)clan_card_request[i]=checked[i];
    donor_size=native_actor_encode(donor.home,donor.avatar,donor_home,sizeof donor_home,&donor_offset);
    if(now>donor_epoch)donor_epoch=now;
    native_actor_destroy(donor);
    account_home_clock(logic,home,0);capture_native(home,avatar);
    // Only after the atomic commit does the stream report one actual material.
    reply(active_messaging,24312,0,clan_card_request,card_request_size);
    CALL(API_printf,int,(const char*,u32,int,int,int,int))(
        "GuestOffline: atomic native donation request=%u stock=%d->%d receiver=%d->%d\n",
        card_request_sequence,before_stock,before_stock-1,before_received,before_received+1);
}
