/* GNR1: native NPC request/cooldown and reciprocal delivery proof. */
static u8 npc_request[2048];
static u32 npc_request_size,npc_request_sequence,npc_request_stamp,npc_request_binding,npc_request_loaded;
static void write_npc_request(void *file,u32 body_hash) {
    if(!npc_request_sequence)return;
    u32 h[9]={0x31524e47u,1,npc_request_size,npc_request_sequence,npc_request_stamp,body_hash,
        checksum(donor_home,donor_size),checksum(npc_request,npc_request_size),0};
    h[8]=checksum((const u8*)h,32);
    if(!donor_size || CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(npc_request,1,npc_request_size,file)!=npc_request_size)
        fail("GuestOffline: NPC request checkpoint write failed");
}
static void read_npc_request(void *file,u32 body_hash) {
    u32 h[9]={0x31524e47u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,32,file)!=32 || h[1]!=1 ||
       h[8]!=checksum((const u8*)h,32) || h[5]!=body_hash || h[2]>sizeof npc_request ||
       !h[3] || h[3]>1000000000u || (!h[2]!=!h[4]) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(npc_request,1,h[2],file)!=h[2] ||
       h[7]!=checksum(npc_request,h[2]))fail("GuestOffline: invalid NPC request checkpoint; original retained");
    npc_request_size=h[2];npc_request_sequence=h[3];npc_request_stamp=h[4];
    npc_request_binding=h[6];npc_request_loaded=1;
}
