/* Opt-in diagnostic of original donor/receiver primitives on two independent
 * native copies. Not clan membership, persistent inventory or UI fulfillment.
 * Production builds omit this entire file unless the explicit probe flag is set.
 * LogicGameMode allocation 0x128 is pinned by GameMode::enter at 0x926e6.
 * Both original constructors supply their own native default event listeners. */
struct ProbeActor {void *logic,*home,*avatar;};
static struct ProbeActor probe_actor(int high) {
    u8 body[sizeof initial_home];for(u32 i=0;i<sizeof body;++i)body[i]=initial_home[i];
    if(body[0] || body[1] || body[2] || body[3] || body[4] || body[5] || body[6] || body[7]!=1)
        fail("DonationActorProbe: unexpected initial native home identity");
    body[3]=(u8)high;
    for(u32 i=0;i<3;++i) {
        u32 at=INITIAL_AVATAR_OFFSET+2*i;
        if(body[at] || body[at+1]!=1)fail("DonationActorProbe: initial avatar identity changed");
        body[at]=(u8)high;
    }
    void *packet=decode_packet(24101,0,body,sizeof body);
    struct ProbeActor a={0};
    a.home=CALL(0x1932d,void*,(void*))(packet);
    a.avatar=CALL(0x19339,void*,(void*))(packet);dispose(packet);
    if(!a.home || !a.avatar)fail("DonationActorProbe: actor ownership unavailable");
    CALL(0x9a515,void,(void*,int))(a.avatar,3); // Explicit diagnostic NPC seed, not earned human progression.
    if(high) {
        u32 name[5];CALL(0x18dcf9,void,(void*,const char*))(name,"Local Donor");
        CALL(0x99e49,void,(void*,void*))(a.avatar,name);CALL(0x18db2d,void,(void*))(name);
    }
    a.logic=CALL(API_malloc,void*,(u32))(0x128);
    if(!a.logic)fail("DonationActorProbe: actor allocation/ownership failed");
    CALL(0xd68e5,void,(void*,int))(a.logic,0);
    CALL(0xd6d01,void,(void*,void*,void*,int,int,int))(a.logic,a.home,a.avatar,0,-1,0);
    return a;
}
static void probe_actor_destroy(struct ProbeActor a) {
    CALL(0xd68e9,void,(void*))(a.logic);
    CALL(symbol("free"),void,(void*))(a.logic);
}
static struct ProbeActor probe_actor_roundtrip(struct ProbeActor a) {
    u32 stream[16];
    CALL(0x192a71,void,(void*,u32))(stream,65536);
    CALL(0x124cfd,void,(void*,void*))(a.home,stream);
    CALL(0x99e51,void,(void*,void*))(a.avatar,stream);
    CALL(0x193749,void,(void*,int))(stream,0); // OwnHomeData's absent visitor.
    u32 size=CALL(0x192b2d,u32,(void*))(stream);
    if(!size || size>65536)fail("DonationActorProbe: native roundtrip extent invalid");
    void *packet=decode_packet(24101,0,CALL(0x1936fd,const u8*,(void*))(stream),size);
    struct ProbeActor restored={0};
    restored.home=CALL(0x1932d,void*,(void*))(packet);
    restored.avatar=CALL(0x19339,void*,(void*))(packet);
    dispose(packet);CALL(0x19370d,void,(void*))(stream);
    if(!restored.home || !restored.avatar)fail("DonationActorProbe: roundtrip actor missing");
    restored.logic=CALL(API_malloc,void*,(u32))(0x128);
    if(!restored.logic)fail("DonationActorProbe: no roundtrip logic allocation");
    CALL(0xd68e5,void,(void*,int))(restored.logic,0);
    CALL(0xd6d01,void,(void*,void*,void*,int,int,int))(restored.logic,restored.home,restored.avatar,0,-1,0);
    if(FIELD(restored.avatar,0x10)!=FIELD(a.avatar,0x10) || FIELD(restored.avatar,0x14)!=FIELD(a.avatar,0x14))
        fail("DonationActorProbe: roundtrip identity changed");
    probe_actor_destroy(a);return restored;
}
static void donation_actor_probe_case(int rare) {

    struct ProbeActor donor=probe_actor(3),recipient=probe_actor(0);
    void *deck=CALL(0x1255fd,void*,(void*))(donor.home);
    void *card=0;
    int count=CALL(0x16c55,int,(void*))(deck);
    for(int i=0;i<count;++i) {
        void *candidate=CALL(0x16b15,void*,(void*,int))(deck,i);
        void *candidate_data=CALL(0x3755d,void*,(void*))(candidate);
        if(CALL(0x755f1,int,(void*))(candidate_data)==(rare?10:1)){card=candidate;break;}
    }
    if(!card)fail("DonationActorProbe: no initial owned card");
    void *data=CALL(0x3755d,void*,(void*))(card);
    void *received=CALL(0x1262c1,void*,(void*,void*))(recipient.home,data);
    if(!data || !received || CALL(0x755f1,int,(void*))(data)!=(rare?10:1))
        fail("DonationActorProbe: expected card rarity in both original starter profiles");
    int stock_before=(int)FIELD(card,0xc),received_before=(int)FIELD(received,0xc);
    int gold_before=CALL(0x9ab55,int,(void*))(donor.avatar),xp_before=CALL(0x9abc5,int,(void*))(donor.avatar);
    u32 send[9]={0};CALL(0x10399,void,(void*))(send);
    u32 *id=CALL(API_malloc,void*,(u32))(8);if(!id)fail("DonationActorProbe: no target ID");
    id[0]=0;id[1]=1;CALL(0x103dd,void,(void*,void*,void*))(send,id,data);
    CALL(API_printf,int,(const char*,int,int,int,int))(
        "DonationActorProbe: before stock=%d recipient=%d gold=%d cooldown=%d\n",stock_before,received_before,gold_before,
        CALL(0x126a59,int,(void*))(donor.home));
    int sent=CALL(0x10439,int,(void*,void*,int,int))(send,donor.logic,2,0);
    CALL(0x103b9,void,(void*))(send);
    if(sent || (int)FIELD(card,0xc)!=stock_before-1)
        fail("DonationActorProbe: original donor command did not consume exactly one material");
    u32 receive[9]={0};CALL(0x21be9,void,(void*))(receive);
    void *name=CALL(API_malloc,void*,(u32))(20);if(!name)fail("DonationActorProbe: no donor name");
    CALL(0x18dcf9,void,(void*,const char*))(name,"Local Donor");
    FIELD(receive,0x1c)=(u32)name;FIELD(receive,0x20)=(u32)data;
    int accepted=CALL(0x21d1d,int,(void*,void*,int,int))(receive,recipient.logic,2,0);
    CALL(0x21c09,void,(void*))(receive);
    int gold_after=CALL(0x9ab55,int,(void*))(donor.avatar),xp_after=CALL(0x9abc5,int,(void*))(donor.avatar);
    int reward=CALL(0x75649,int,(void*))(data),reward_xp=CALL(0x75651,int,(void*))(data);
    if(accepted || (int)FIELD(received,0xc)!=received_before+1 ||
       gold_after-gold_before!=reward || xp_after-xp_before!=reward_xp ||
       stock_before+received_before!=(int)FIELD(card,0xc)+(int)FIELD(received,0xc))
        fail("DonationActorProbe: original transfer failed conservation/native reward check");
    CALL(API_printf,int,(const char*,int,int,int,int,int,int,int,int))(
        "DonationActorProbe: native203/211 conserved stock=%d->%d recipient=%d->%d gold=%d->%d xp=%d->%d\n",
        stock_before,FIELD(card,0xc),received_before,FIELD(received,0xc),gold_before,gold_after,xp_before,xp_after);
    donor=probe_actor_roundtrip(donor);recipient=probe_actor_roundtrip(recipient);
    card=CALL(0x1262c1,void*,(void*,void*))(donor.home,data);
    received=CALL(0x1262c1,void*,(void*,void*))(recipient.home,data);
    if(!card || !received || FIELD(card,0xc)!=0 || FIELD(received,0xc)!=(u32)(received_before+1) ||
       CALL(0x9ab55,int,(void*))(donor.avatar)!=gold_after || CALL(0x9abc5,int,(void*))(donor.avatar)!=xp_after)
        fail("DonationActorProbe: native serialization lost stock or rewards");
    CALL(API_puts,int,(const char*))("DonationActorProbe: both native home/avatar roundtrips retained stock and rewards");
    // Original 203 reports an empty-stock failure through its listener but
    // still returns zero. Never treat that return as proof a card was donated.
    CALL(0x10399,void,(void*))(send);
    id=CALL(API_malloc,void*,(u32))(8);if(!id)fail("DonationActorProbe: no second target ID");
    id[0]=0;id[1]=1;CALL(0x103dd,void,(void*,void*,void*))(send,id,data);
    int second=CALL(0x10439,int,(void*,void*,int,int))(send,donor.logic,2,0);
    CALL(0x103b9,void,(void*))(send);
    if(second || FIELD(card,0xc)!=0 || FIELD(received,0xc)!=(u32)(received_before+1) ||
       CALL(0x9ab55,int,(void*))(donor.avatar)!=gold_after || CALL(0x9abc5,int,(void*))(donor.avatar)!=xp_after)
        fail("DonationActorProbe: empty stock was consumed or rewarded again");
    CALL(API_puts,int,(const char*))("DonationActorProbe: empty-stock 203 returns zero with no transfer/reward; state checks required");
    probe_actor_destroy(recipient);probe_actor_destroy(donor);
    CALL(API_puts,int,(const char*))("DonationActorProbe: independent actors cleaned; live player unchanged");
}
static void donation_actor_probe(void) {
    static int done;if(done)return;done=1;
    u32 size=saved_size,before=checksum(saved_home,saved_size),offset=avatar_offset;
    donation_actor_probe_case(0);donation_actor_probe_case(1);
    if(size!=saved_size || offset!=avatar_offset || before!=checksum(saved_home,saved_size))
        fail("DonationActorProbe: live saved home unexpectedly changed");
    CALL(API_puts,int,(const char*))("DonationActorProbe: common/rare native transfers verified, live checkpoint body unchanged");
}
