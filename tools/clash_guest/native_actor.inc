/* Original independent home/avatar ownership, including native listeners. */
struct NativeActor {void *logic,*home,*avatar;};
static struct NativeActor native_actor(const u8 *data,u32 size) {
    void *packet=decode_packet(24101,0,data,size);struct NativeActor a={0};
    a.home=CALL(0x1932d,void*,(void*))(packet);
    a.avatar=CALL(0x19339,void*,(void*))(packet);dispose(packet);
    if(!a.home || !a.avatar)fail("GuestOffline: native actor has no home/avatar");
    // GameMode::enter 0x926e6 allocates exactly 0x128 for LogicGameMode.
    a.logic=CALL(API_malloc,void*,(u32))(0x128);
    if(!a.logic)fail("GuestOffline: native actor allocation failed");
    CALL(0xd68e5,void,(void*,int))(a.logic,0);
    CALL(0xd6d01,void,(void*,void*,void*,int,int,int))(a.logic,a.home,a.avatar,0,-1,0);
    return a;
}
static void native_actor_destroy(struct NativeActor a) {
    CALL(0xd68e9,void,(void*))(a.logic);CALL(symbol("free"),void,(void*))(a.logic);
}
static u32 native_actor_encode(void *home,void *avatar,u8 *output,u32 capacity,u32 *offset) {
    u32 stream[16];CALL(0x192a71,void,(void*,u32))(stream,capacity);
    CALL(0x124cfd,void,(void*,void*))(home,stream);*offset=CALL(0x192b2d,u32,(void*))(stream);
    CALL(0x99e51,void,(void*,void*))(avatar,stream);
    u32 size=CALL(0x192b2d,u32,(void*))(stream);
    const u8 *data=CALL(0x1936fd,const u8*,(void*))(stream);
    if(size>=capacity || *offset>=size)fail("GuestOffline: native actor serialization exceeds bound");
    for(u32 i=0;i<size;++i)output[i]=data[i];output[size++]=0;
    CALL(0x19370d,void,(void*))(stream);return size;
}
