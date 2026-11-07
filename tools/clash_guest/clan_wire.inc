/* Pinned original CreateAlliance wire bounds, without native pointers. */
struct ClanFields {u32 name_at,name_size,desc_at,desc_size;int badge,type,score,region;};
static int clan_ref(struct GuestReader *r,int expected) {
    int type=turn_vint(r);if(!type)return -1;
    int index=turn_vint(r);
    if(type!=expected || index<0 || index>=1000000)fail("GuestOffline: invalid clan data reference");
    return index;
}
static struct ClanFields clan_fields(const u8 *data,u32 size) {
    struct GuestReader r={data,size,0,0,0};struct ClanFields f;
    f.name_size=turn_int(&r);f.name_at=r.at;
    if(!f.name_size || f.name_size>60 || f.name_size>r.size-r.at)
        fail("GuestOffline: invalid clan name extent");
    for(u32 i=0;i<f.name_size;++i)if(data[r.at+i]<32 || data[r.at+i]==127)
        fail("GuestOffline: clan name contains control characters");
    r.at+=f.name_size;
    f.desc_size=turn_int(&r);f.desc_at=r.at;
    if(f.desc_size==0xffffffffu)f.desc_size=0;
    if(f.desc_size>800 || f.desc_size>r.size-r.at)fail("GuestOffline: invalid clan description extent");
    r.at+=f.desc_size;f.badge=clan_ref(&r,16);f.type=turn_vint(&r);f.score=turn_vint(&r);f.region=clan_ref(&r,57);
    if(f.badge<0 || f.type<1 || f.type>3 || f.score<0 || f.score>100000 || r.at!=r.size)
        fail("GuestOffline: unsupported clan creation fields");
    return f;
}
