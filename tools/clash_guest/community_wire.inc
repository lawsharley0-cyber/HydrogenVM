static const char community_message[]="Offline clanmate. Tap my name to view or invite me.";
static void community_literal(struct GuestReader *r,const char *text,u32 length) {
    if(turn_int(r)!=length || length>r->size-r->at)fail("GuestOffline: invalid NPC profile text extent");
    for(u32 i=0;i<length;++i)if(turn_byte(r)!=(u8)text[i])fail("GuestOffline: invalid NPC profile text");
}
static void community_entry_wire(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now) {
    struct GuestReader r={data,size,0,0,0};
    const int expected[]={2,4,1,3,1,3,1};
    for(u32 i=0;i<7;++i)if(turn_vint(&r)!=expected[i])fail("GuestOffline: invalid local NPC profile identity");
    community_literal(&r,"Local Donor",11);
    int level=turn_vint(&r),role=turn_vint(&r);
    if(level<3 || level>12 || role<0 || role>4)fail("GuestOffline: invalid NPC profile attributes");
    u32 age_at=r.at;
    if(turn_vint(&r)!=0)fail("GuestOffline: invalid NPC profile stored age");
    u32 age_end=r.at;
    if(turn_bool(&r))fail("GuestOffline: removed local NPC profile message");
    community_literal(&r,community_message,sizeof community_message-1);
    if(r.at!=r.size || !stamp)fail("GuestOffline: invalid NPC profile entry extent");
    sector_bytes(w,data,age_at);u32 age=now>stamp?now-stamp:0;
    sector_vint(w,(int)(age>2147483647u?2147483647u:age));sector_bytes(w,data+age_end,size-age_end);
}
