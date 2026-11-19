/* Pinned native clan-mail entry bounds and timestamp-relative age. */
static void local_mail_entry(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now) {
    struct GuestReader r={data,size,0,0,0};
    if(turn_vint(&r)!=6 || turn_int(&r)!=0)fail("GuestOffline: invalid local mail type/identity");
    u32 serial=turn_int(&r);
    if(!serial || serial>local_mail_sequence || turn_int(&r)!=0 || turn_int(&r)!=1)
        fail("GuestOffline: invalid local mail author/sequence");
    clan_skip_string(&r,60,1);int sender_metadata=turn_vint(&r);
    // The original constructor initializes this opaque field to zero.
    if(sender_metadata!=0)fail("GuestOffline: unsupported local mail sender metadata");
    u32 age_at=r.at;
    if(turn_vint(&r)!=0)fail("GuestOffline: invalid stored mail age");
    u32 age_end=r.at;
    if(turn_bool(&r) || !turn_bool(&r))fail("GuestOffline: unsupported stored mail flags");
    clan_skip_string(&r,1024,1);
    if(!turn_bool(&r) || turn_int(&r)!=0 || turn_int(&r)!=1)
        fail("GuestOffline: invalid local mail home identity");
    clan_skip_string(&r,1024,1);
    if(turn_int(&r)!=1)fail("GuestOffline: invalid local mail clan identity");
    u32 clan=turn_int(&r);
    if(!clan || clan>1000000)fail("GuestOffline: invalid local mail clan serial");
    clan_skip_string(&r,60,1);
    if(clan_ref(&r,16)<0 || r.at!=r.size || !stamp)fail("GuestOffline: invalid local mail record extent");
    sector_bytes(w,data,age_at);u32 age=now>stamp?now-stamp:0;
    sector_vint(w,(int)(age>2147483647u?2147483647u:age));
    sector_bytes(w,data+age_end,size-age_end);
}
