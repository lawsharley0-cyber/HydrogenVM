/* Original initial-name command, not a synthetic profile edit or paid rename.
 * Native ChangeAvatarNameMessage 10212 and command 201 retain their original
 * validation/execution/UI notification. No resources are charged or granted.
 */
static u8 initial_name[60];
static u32 initial_name_length;
static int initial_name_pending;
static const u8 *saved_name(u32 *length,int *named) {
    struct GuestReader r={saved_home+avatar_offset,saved_size-avatar_offset-1,0,0,0};
    for(int i=0;i<6;++i)turn_vint(&r);
    *length=turn_int(&r);
    if(*length>r.size-r.at)fail("GuestOffline: invalid saved name extent");
    const u8 *name=r.data+r.at;r.at+=*length;*named=turn_bool(&r);
    return name;
}
static void change_initial_name(void *self,void *message) {
    if(initial_name_pending)fail("GuestOffline: duplicate pending initial name");
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    u32 old_length;int named;saved_name(&old_length,&named);
    if(named)fail("GuestOffline: paid name change is not implemented");
    struct GuestReader r={(const u8*)FIELD(message,8+0x1c),CALL(0x198d19,u32,(void*))(message),0,0,0};
    u32 length=turn_int(&r);
    if(!length || length>sizeof initial_name || length>r.size-r.at)
        fail("GuestOffline: invalid initial name extent");
    for(u32 i=0;i<length;++i) {
        u8 ch=turn_byte(&r);
        if(ch<32 || ch==127)fail("GuestOffline: initial name contains control characters");
        initial_name[i]=ch;
    }
    if(turn_byte(&r)!=1 || r.at!=r.size)fail("GuestOffline: invalid initial name request flags");
    initial_name_length=length;initial_name_pending=1;
    u8 bytes[128];struct SectorWriter w={bytes,0,sizeof bytes};
    sector_vint(&w,201);
    for(int shift=24;shift>=0;shift-=8)sector_byte(&w,(u8)(length>>shift));
    sector_bytes(&w,initial_name,length);
    // Native decoder 0x122958 reads a fixed BE integer, then a bool; the
    // avatar's similarly named field uses VInt and is not this wire schema.
    for(int i=0;i<4;++i)sector_byte(&w,0);
    sector_byte(&w,1);
    const int metadata[]={0,-1,-1,0,1}; // server ID, LogicCommand base metadata
    for(int i=0;i<5;++i)sector_vint(&w,metadata[i]);
    reply(self,24111,0,bytes,w.size);
    CALL(API_puts,int,(const char*))("GuestOffline: supplied original initial-name command");
}
static void name_echo(struct GuestReader *r) {
    const u32 length=turn_int(r);
    if(!length || length>sizeof initial_name || length>r->size-r->at)
        fail("GuestOffline: invalid initial-name echo extent");
    const u8 *name=r->data+r->at;r->at+=length;
    if(turn_int(r)!=0 || turn_byte(r)!=1 || turn_vint(r)!=0)
        fail("GuestOffline: unsupported name command state");
    int tick=turn_vint(r),sequence=turn_vint(r);
    int high=turn_vint(r),low=turn_vint(r);
    if(tick< -1 || sequence< -1 || high!=0 || low!=1)
        fail("GuestOffline: invalid initial-name command metadata");
    if(initial_name_pending) {
        if(length!=initial_name_length)fail("GuestOffline: initial-name echo mismatch");
        for(u32 i=0;i<length;++i)if(name[i]!=initial_name[i])fail("GuestOffline: initial-name echo mismatch");
    }
    // It has already executed in the original client. Check its real result
    // before accepting the echo, including a replay matching a persisted name.
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    u32 actual_length;int named;const u8 *actual=saved_name(&actual_length,&named);
    if(!named || length!=actual_length)fail("GuestOffline: native name command did not apply");
    for(u32 i=0;i<length;++i)if(name[i]!=actual[i])fail("GuestOffline: native name differs from command");
    initial_name_pending=0;
    CALL(API_puts,int,(const char*))("GuestOffline: native initial name applied and checkpointed");
}
