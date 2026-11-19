/* Original DonateStreamEntry type 1, bounded to this actual local donor. */
struct CardRequestInfo {u32 donors_at,donors_end,donors,card_count,capacity;int type,index;};
static struct CardRequestInfo request_entry_for(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now,
        u32 space,u32 sequence,int author,int donor,u32 limit) {
    struct CardRequestInfo info={0};
    struct GuestReader r={data,size,0,0,0};
    if(turn_vint(&r)!=1 || (u32)turn_vint(&r)!=space || (u32)turn_vint(&r)!=sequence)
        fail("GuestOffline: invalid local card request identity");
    for(int i=0;i<2;++i)if(turn_vint(&r)!=author || turn_vint(&r)!=1)
        fail("GuestOffline: invalid local card request author");
    clan_skip_string(&r,60,1);int level=turn_vint(&r),role=turn_vint(&r);
    if(level<1 || level>12 || role<1 || role>4)fail("GuestOffline: invalid local card request attributes");
    u32 age_at=r.at;
    if(turn_vint(&r)!=0)fail("GuestOffline: invalid stored card request age");
    u32 age_end=r.at;
    if(turn_bool(&r))fail("GuestOffline: unsupported removed card request");
    u32 capacity=turn_int(&r);info.donors_at=r.at;u32 donors=turn_int(&r);
    int donated_type=0,donated_index=0;
    if(donors==1) {
        if(turn_int(&r)!=donor || turn_int(&r)!=1)
            fail("GuestOffline: invalid local request donor identity/count");
        info.card_count=turn_int(&r);
        if(!info.card_count || info.card_count>limit)fail("GuestOffline: invalid donated card count");
        for(u32 i=0;i<info.card_count;++i) {
            int type=turn_vint(&r),index=turn_vint(&r);
            if(i && (type!=donated_type || index!=donated_index))fail("GuestOffline: mixed donated cards");
            donated_type=type;donated_index=index;
        }
    }
    info.donors_end=r.at;
    if(capacity<1 || capacity>1000 || donors>1 || turn_vint(&r)!=1)
        fail("GuestOffline: unsupported local card request capacity/donors/wishlist");
    int type=turn_vint(&r),index=turn_vint(&r);
    if((type!=26 && type!=28) || index<0 || index>=200)
        fail("GuestOffline: invalid local card request spell reference");
    if(donors && (donated_type!=type || donated_index!=index))
        fail("GuestOffline: donated card differs from request wishlist");
    if(turn_bool(&r))clan_skip_string(&r,1024,0);
    if(r.at!=r.size || !stamp)fail("GuestOffline: invalid local card request extent");
    sector_bytes(w,data,age_at);u32 age=now>stamp?now-stamp:0;
    sector_vint(w,(int)(age>2147483647u?2147483647u:age));
    sector_bytes(w,data+age_end,size-age_end);
    info.donors=donors;info.capacity=capacity;info.type=type;info.index=index;return info;
}
static struct CardRequestInfo card_request_entry(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now) {
    return request_entry_for(w,data,size,stamp,now,1,card_request_sequence,0,3,1);
}
static struct CardRequestInfo npc_request_entry(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now,u32 sequence) {
    return request_entry_for(w,data,size,stamp,now,3,sequence,3,0,10);
}
