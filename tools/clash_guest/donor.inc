/* One explicitly local NPC with finite starter stock. No online human claim
 * and no automatic stock refill. Native transfers are in donation.inc. */
static void validate_donor(void) {
    if(!donor_size)return;
    if(donor_clan!=clan_serial || clan_status<2 || clan_status>4)
        fail("GuestOffline: saved local donor has no matching clan");
    void *packet=decode_packet(24101,0,donor_home,donor_size);
    void *home=(void*)FIELD(packet,0x30),*avatar=(void*)FIELD(packet,0x34);
    void *home_id=home?(void*)FIELD(home,8):0;
    if(!home_id || !avatar || FIELD(home_id,0)!=3 || FIELD(home_id,4)!=1 ||
       FIELD(avatar,0x10)!=3 || FIELD(avatar,0x14)!=1)
        fail("GuestOffline: saved local donor identity mismatch");
    const u32 identity_getters[]={0x99e2du,0x99e31u,0x99e41u};
    for(u32 i=0;i<3;++i) {
        void *identity=CALL(identity_getters[i],void*,(void*))(avatar);
        if(!identity || FIELD(identity,0)!=3 || FIELD(identity,4)!=1)
            fail("GuestOffline: saved local donor account/home identity mismatch");
    }
    void *id=CALL(0x9ae01,void*,(void*))(avatar);
    donor_role=CALL(0x9ae41,int,(void*))(avatar);
    if((donor_present?(!id || FIELD(id,0)!=1 || FIELD(id,4)!=donor_clan || donor_role<1 || donor_role>4):id!=0) ||
       CALL(0x9abcd,int,(void*))(avatar)<3 || CALL(0x9abcd,int,(void*))(avatar)>12)
        fail("GuestOffline: saved local donor membership mismatch");
    if(donor_present)check_native_clan_name(avatar);
    void *name=CALL(0x99e45,void*,(void*))(avatar);
    const char expected[]="Local Donor";
    if(!name || CALL(0x18f4bd,u32,(void*))(name)!=sizeof expected-1)
        fail("GuestOffline: saved local donor name extent mismatch");
    const u8 *bytes=CALL(0x19c099,const u8*,(void*))(name);
    for(u32 i=0;i<sizeof expected-1;++i)if(!bytes || bytes[i]!=(u8)expected[i])
        fail("GuestOffline: saved local donor name mismatch");
    u32 offset=0;static u8 checked[65536];
    u32 size=native_actor_encode(home,avatar,checked,sizeof checked,&offset);
    if(offset!=donor_offset || size!=donor_size || checksum(checked,size)!=checksum(donor_home,donor_size))
        fail("GuestOffline: saved local donor native representation is not canonical");
    dispose(packet);
}
static void sync_donor(void) {
    if(!clan_status) {donor_size=0;donor_offset=0;donor_clan=0;donor_epoch=0;donor_present=0;donor_role=0;
        donor_inventory_count=0;donor_inventory_body_hash=0;donor_inventory_loaded=0;return;}
    if(clan_status!=2)return;
    if(donor_size && !donor_present)return; // Durable kick, not an invitation to reseed/rejoin.
    void *request=decode_packet(14301,0,clan_request,clan_request_size);
    if(!donor_size) {
        // An authored NPC starts from the pinned original starter collection,
        // with one material per starter card, level 3, 100 Gold and 0 trophies.
        // This seed is created once per paid clan, never on ordinary relaunch.
        u8 seed[sizeof initial_home];for(u32 i=0;i<sizeof seed;++i)seed[i]=initial_home[i];
        if(sizeof seed<8 || seed[3] || seed[7]!=1)fail("GuestOffline: invalid local donor seed home");
        seed[3]=3;
        for(u32 i=0;i<3;++i) {
            u32 at=INITIAL_AVATAR_OFFSET+2*i;
            if(seed[at] || seed[at+1]!=1)fail("GuestOffline: invalid local donor seed avatar");
            seed[at]=3;
        }
        struct NativeActor a=native_actor(seed,sizeof seed);
        u32 name[5];CALL(0x18dcf9,void,(void*,const char*))(name,"Local Donor");
        CALL(0x99e49,void,(void*,void*))(a.avatar,name);CALL(0x18db2d,void,(void*))(name);
        CALL(0x9a5d5,void,(void*,int))(a.avatar,1); // Native profile must display its authored NPC name.
        CALL(0x9a515,void,(void*,int))(a.avatar,3);
        // Original JoinAllianceCommand 209, create flag false: ordinary member,
        // no creation charge. Borrowed fields stay owned by request/stack.
        u32 command[11]={0},id[2]={1,clan_serial};CALL(0xe5781,void,(void*))(command);
        FIELD(command,0x1c)=(u32)id;FIELD(command,0x20)=FIELD(request,0x38);
        FIELD(command,0x24)=FIELD(request,0x30);BYTE(command,0x28)=0;
        if(CALL(0xe5875,int,(void*,void*,int,int))(command,a.logic,2,0)!=0)
            fail("GuestOffline: native local donor join failed");
        // Clear borrowed pointers before the original command destructor.
        FIELD(command,0x1c)=0;FIELD(command,0x20)=0;FIELD(command,0x24)=0;
        CALL(0xe57a9,void,(void*))(command);
        donor_size=native_actor_encode(a.home,a.avatar,donor_home,sizeof donor_home,&donor_offset);
        donor_clan=clan_serial;donor_epoch=wall_epoch();donor_present=1;donor_role=1;native_actor_destroy(a);
        CALL(API_puts,int,(const char*))("GuestOffline: seeded finite Local Donor through original clan join; no player grant");
    } else {
        if(donor_clan!=clan_serial)fail("GuestOffline: local donor belongs to a different clan");
        void *packet=decode_packet(24101,0,donor_home,donor_size),*avatar=(void*)FIELD(packet,0x34);
        CALL(0x9ae35,void,(void*,void*))(avatar,(void*)FIELD(request,0x38));
        donor_size=native_actor_encode((void*)FIELD(packet,0x30),avatar,donor_home,sizeof donor_home,&donor_offset);
        dispose(packet);
    }
    dispose(request);
    validate_donor_inventory();
}
static void encode_donor_member(void *stream) {
    void *packet=decode_packet(24101,0,donor_home,donor_size),*avatar=(void*)FIELD(packet,0x34);
    u32 member[19]={0};
    FIELD(member,0x10)=(u32)CALL(0x99e2d,void*,(void*))(avatar);FIELD(member,0x14)=FIELD(member,0x10);
    FIELD(member,0x1c)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(member,0x20)=(u32)CALL(0x9ae41,int,(void*))(avatar);FIELD(member,0x24)=(u32)CALL(0x9abcd,int,(void*))(avatar);
    FIELD(member,0x28)=FIELD(avatar,0x34);FIELD(member,0x34)=2;
    FIELD(member,0x48)=(u32)CALL(0x9b4d1,void*,(void*))(avatar);
    CALL(0x37d85,void,(void*,void*))(member,stream);dispose(packet);
}
