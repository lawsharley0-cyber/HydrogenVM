/* Original native reward generation, durable immutable command journal and
 * native execution qualification. No substitute reward tables or fake grants.
 */
static void *claim_chest(void *home,int kind,int id) {
    if(kind==1)return CALL(0x125645,void*,(void*,int))(home,id);
    return (void*)FIELD(home,kind==2?0x78:0x90);
}
static void prepare_reward(void *home,void *avatar) {
    if(pending_size || !requested_kind || !BYTE(home,0xb4))return;
    int kind=requested_kind,id=requested_id;
    void *chest=claim_chest(home,kind,id);
    // The native command can execute before its EndClientTurn reaches this
    // adapter. Wait for the matching request rather than interpreting another
    // source's in-flight claim as this one.
    if(!chest || !BYTE(chest,0x18))return;
    serialize_native(home,avatar,0);
    void *checkpoint=decode_packet(24101,0,saved_home,saved_size);
    void *copy_home=(void*)FIELD(checkpoint,0x30),*copy_avatar=(void*)FIELD(checkpoint,0x34);
    void *copy_chest=claim_chest(copy_home,kind,id);
    if(!copy_chest || FIELD(copy_chest,0)!=FIELD(chest,0) || FIELD(copy_chest,8)!=FIELD(chest,8) || !BYTE(copy_chest,0x18))
        fail("GuestOffline: claim checkpoint source mismatch");
    // Only this validated claim is reverted in a temporary decoded copy.
    // Unlock state, native gem charge, deck and all other fields remain intact.
    BYTE(copy_chest,0x18)=0;
    u32 randomizer[2],command[11],stream[16];
    CALL(0x132f75,void,(void*))(randomizer);
    u32 seed=checksum(saved_home,saved_size)^(u32)id;
    void *reward=CALL(0x132f99,void*,(void*,void*,u32,void*))(randomizer,chest,seed,home);
    if(!reward)fail("GuestOffline: original chest randomizer failed");
    CALL(0x81fed,void,(void*))(command);
    CALL(0x829c1,void,(void*,int))(command,id);
    CALL(0x829c5,void,(void*,int))(command,kind);
    CALL(0x82841,void,(void*,void*))(command,reward);
    CALL(0x192a71,void,(void*,u32))(stream,65536);
    CALL(0x6d8c5,void,(void*,void*))(stream,command);
    u32 size=CALL(0x192b2d,u32,(void*))(stream);
    const u8 *data=CALL(0x1936fd,const u8*,(void*))(stream);
    if(size<3 || size>sizeof pending_reward)fail("GuestOffline: native reward encoding exceeds bound");
    for(u32 i=0;i<size;++i)pending_reward[i]=data[i];
    CALL(0x19370d,void,(void*))(stream);
    CALL(0x82011,void,(void*))(command);
    CALL(0x132f79,void,(void*))(randomizer);
    pending_size=size;pending_kind=kind;pending_id=id;reward_applied=0;
    serialize_native(copy_home,copy_avatar,0);
    write_save();dispose(checkpoint);
    requested_kind=0;requested_id=0;
    CALL(API_printf,int,(const char*,int))("GuestOffline: reward journal committed, bytes=%d\n",(int)pending_size);
    if(logged_in && active_messaging && BYTE(active_messaging,0xe4))
        reply(active_messaging,24111,0,pending_reward,pending_size);
}
extern int guest_original_reward_execute(void*,void*,int,int);
int guest_reward_execute(void *command,void *logic,int mode,int flags) {
    int source=(int)FIELD(command,0x24),id=(int)FIELD(command,0x20);
    int result=guest_original_reward_execute(command,logic,mode,flags);
    if(result==0 && pending_size && source==pending_kind && (source!=1 || id==pending_id)) {
        reward_applied=1;
        CALL(API_puts,int,(const char*))("GuestOffline: original reward execute returned success");
    }
    return result;
}
