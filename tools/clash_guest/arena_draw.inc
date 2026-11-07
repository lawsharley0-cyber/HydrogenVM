/* The native NPC draw path skips both win/loss setters. Complete its own
 * checkpoint before the result becomes visible, retaining original rewards. */
static void persist_arena_draw(void *battle) {
    if(!BYTE(battle,0xb4) || FIELD(battle,0x74)!=0xffffffffu ||
       CALL(0x14089,void*,(void*))(battle) || CALL(0x140b5,void*,(void*))(battle))
        fail("GuestOffline: incomplete or decisive draw checkpoint");
    void *mode=(void*)FIELD(battle,0),*live=0;
    if(!mode)fail("GuestOffline: draw has no native battle mode");
    // GameMode::getClientAvatar returns the retained home-screen avatar,
    // whose wallet predates the paid sector. Resolve the battle's own avatar.
    for(int i=0;i<2;++i) {
        void *candidate=CALL(0xd7bd9,void*,(void*,int))(mode,i);
        if(candidate && FIELD(candidate,0x10)==0 && FIELD(candidate,0x14)==1) {
            if(live)fail("GuestOffline: duplicate draw checkpoint owner");
            live=candidate;
        }
    }
    if(!live)
        fail("GuestOffline: draw checkpoint owner mismatch");
    serialize_native(0,live,0);
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30),*copy=(void*)FIELD(owner,0x34);
    if(!home || !copy)fail("GuestOffline: invalid decoded draw checkpoint");
    credit_arena_crowns(copy);
    CALL(0x9b709,void,(void*))(copy);
    capture_native(home,copy);dispose(owner);
    CALL(API_puts,int,(const char*))("GuestOffline: persisted original arena draw; no win/loss or winner chest");
}
