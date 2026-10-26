/* Original native chat-entry boundaries and timestamp-relative age. */
static void clan_skip_string(struct GuestReader *r,u32 maximum,int required) {
    u32 size=turn_int(r);
    if(size==0xffffffffu && !required)return;
    if((required && !size) || size>maximum || size>r->size-r->at)
        fail("GuestOffline: invalid conversation string");
    r->at+=size;
}
static void clan_chat_entry(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now) {
    struct GuestReader r={data,size,0,0,0};
    if(turn_vint(&r)!=2 || turn_vint(&r)!=0)fail("GuestOffline: invalid conversation type/identity");
    int serial=turn_vint(&r);
    if(serial<1 || (u32)serial>clan_chat_sequence)fail("GuestOffline: invalid conversation sequence");
    for(int i=0;i<2;++i)if(turn_vint(&r)!=0 || turn_vint(&r)!=1)
        fail("GuestOffline: conversation author differs from local player");
    clan_skip_string(&r,60,1);
    int level=turn_vint(&r),role=turn_vint(&r);
    if(level<1 || level>12 || role!=2)fail("GuestOffline: invalid conversation author attributes");
    u32 age_at=r.at;
    if(turn_vint(&r)!=0)fail("GuestOffline: stored conversation age is not relative to its timestamp");
    u32 age_end=r.at;
    if(turn_bool(&r))fail("GuestOffline: unsupported removed conversation entry");
    clan_skip_string(&r,512,1);
    if(r.at!=r.size || !stamp)fail("GuestOffline: invalid conversation record extent/timestamp");
    sector_bytes(w,data,age_at);
    u32 elapsed=now>stamp?now-stamp:0;
    sector_vint(w,(int)(elapsed>2147483647u?2147483647u:elapsed));
    sector_bytes(w,data+age_end,size-age_end);
}
