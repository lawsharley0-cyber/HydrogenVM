/* GFR1: one atomic Friendly Challenge and its explicit local NPC escrow.
 * State 1 is paid/pending, 2 accepted, 3 completed, 4 cancelled/refunded,
 * 5 abandoned after acceptance. Only states 1 and 2 are active. */
static u8 friendly_entry[2048],friendly_result[32768];
static u32 friendly_sequence,friendly_state,friendly_stamp,friendly_accepted_stamp;
static u32 friendly_entry_size,friendly_result_size,friendly_echo_crc;
static int friendly_gold_before,friendly_gold_after,friendly_free_before,friendly_free_after;
static int friendly_npc_before,friendly_npc_after;
static void write_friendly(void *file,u32 body_hash) {
    if(!friendly_sequence)return;
    u32 h[20]={0x31524647u,1,friendly_sequence,friendly_state,friendly_stamp,
        friendly_entry_size,friendly_result_size,(u32)friendly_gold_before,
        (u32)friendly_gold_after,(u32)friendly_free_before,(u32)friendly_free_after,
        (u32)friendly_npc_before,(u32)friendly_npc_after,body_hash,friendly_echo_crc,
        checksum(friendly_entry,friendly_entry_size),checksum(friendly_result,friendly_result_size),
        friendly_accepted_stamp,0,0};
    h[19]=checksum((const u8*)h,76);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(friendly_entry,1,friendly_entry_size,file)!=friendly_entry_size ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(friendly_result,1,friendly_result_size,file)!=friendly_result_size)
        fail("GuestOffline: friendly checkpoint write failed");
}
static void read_friendly(void *file,u32 body_hash) {
    u32 h[20]={0x31524647u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,76,file)!=76 ||
       h[19]!=checksum((const u8*)h,76) || h[1]!=1 || !h[2] || h[2]>1000000000 ||
       h[3]<1 || h[3]>5 || !h[4] || h[5]<20 || h[5]>sizeof friendly_entry ||
       h[6]>sizeof friendly_result || ((h[3]==3)!=(h[6]!=0)) || h[13]!=body_hash ||
       h[7]>2000000000u || h[8]>2000000000u || h[9]>h[7] || h[10]>h[8] ||
       h[7]-h[8]!=5 || h[11]!=5 || h[12]>5 || h[18] ||
       (h[3]==4 && h[12]!=5) || (h[3]!=4 && h[3]!=1 && h[12]!=0) ||
       (h[3]==1 && h[12]!=5) || ((h[3]==2 || h[3]==3 || h[3]==5) && !h[17]) ||
       ((h[3]==1 || h[3]==4) && h[17]))
        fail("GuestOffline: invalid friendly checkpoint header; original retained");
    if(CALL(s_read,u32,(void*,u32,u32,void*))(friendly_entry,1,h[5],file)!=h[5] ||
       CALL(s_read,u32,(void*,u32,u32,void*))(friendly_result,1,h[6],file)!=h[6] ||
       h[15]!=checksum(friendly_entry,h[5]) || h[16]!=checksum(friendly_result,h[6]))
        fail("GuestOffline: invalid friendly checkpoint body; original retained");
    friendly_sequence=h[2];friendly_state=h[3];friendly_stamp=h[4];friendly_entry_size=h[5];
    friendly_result_size=h[6];friendly_gold_before=(int)h[7];friendly_gold_after=(int)h[8];
    friendly_free_before=(int)h[9];friendly_free_after=(int)h[10];
    friendly_npc_before=(int)h[11];friendly_npc_after=(int)h[12];friendly_echo_crc=h[14];
    friendly_accepted_stamp=h[17];
}
