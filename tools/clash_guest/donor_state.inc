/* GDN1: finite, explicitly simulated clan member; atomic with the human home.
 * This is original serialized home/avatar data, never a per-login refill. */
static u8 donor_home[65536];
static u32 donor_size,donor_offset,donor_clan,donor_epoch;
static void write_donor(void *file,u32 body_hash) {
    if(!donor_size)return;
    u32 h[9]={0x314e4447u,1,donor_clan,donor_size,donor_offset,donor_epoch,body_hash,
        checksum(donor_home,donor_size),0};
    h[8]=checksum((const u8*)h,32);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(donor_home,1,donor_size,file)!=donor_size)
        fail("GuestOffline: local donor checkpoint write failed");
}
static void read_donor(void *file,u32 body_hash) {
    u32 h[9]={0x314e4447u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,32,file)!=32 || h[1]!=1 ||
       !h[2] || h[2]>1000000 || h[3]<16 || h[3]>sizeof donor_home ||
       h[4]>=h[3] || h[3]-h[4]<8 || !h[5] || h[6]!=body_hash ||
       h[8]!=checksum((const u8*)h,32) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(donor_home,1,h[3],file)!=h[3] ||
       h[7]!=checksum(donor_home,h[3]))
        fail("GuestOffline: invalid local donor checkpoint; original retained");
    donor_clan=h[2];donor_size=h[3];donor_offset=h[4];donor_epoch=h[5];
}
