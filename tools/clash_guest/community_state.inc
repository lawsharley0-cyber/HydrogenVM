/* GCM1 binds a reachable local NPC profile and weekly points to both actors. */
#include "community_math.inc"
static u8 community_entry[256];
static u32 community_clan,community_week,community_human_sent,community_npc_sent;
static u32 community_human_received,community_npc_received,community_stamp,community_size;
static u32 community_binding,community_loaded;
static void write_community(void *file,u32 body_hash) {
    if(!community_clan)return;
    u32 h[15]={0x314d4347u,1,community_clan,community_week,community_human_sent,community_npc_sent,
        community_human_received,community_npc_received,community_stamp,community_size,
        body_hash,checksum(donor_home,donor_size),checksum(community_entry,community_size),1,0};
    h[14]=checksum((const u8*)h,56);
    if(!donor_size || CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(community_entry,1,community_size,file)!=community_size)
        fail("GuestOffline: clan community checkpoint write failed");
}
static void read_community(void *file,u32 body_hash) {
    u32 h[15]={0x314d4347u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,56,file)!=56 || h[1]!=1 ||
       !h[2] || h[2]>1000000u || !h[3] || community_week_start(h[3])!=h[3] ||
       h[4]>1000000000u || h[5]>1000000000u || h[4]!=h[7] || h[5]!=h[6] ||
       !h[8] || h[9]<16 || h[9]>sizeof community_entry || h[10]!=body_hash || h[13]!=1 ||
       h[14]!=checksum((const u8*)h,56) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(community_entry,1,h[9],file)!=h[9] ||
       h[12]!=checksum(community_entry,h[9]))
        fail("GuestOffline: invalid clan community checkpoint; original retained");
    community_clan=h[2];community_week=h[3];community_human_sent=h[4];community_npc_sent=h[5];
    community_human_received=h[6];community_npc_received=h[7];community_stamp=h[8];community_size=h[9];
    community_binding=h[11];community_loaded=1;
}
