/* One actual local member's latest request, atomic with native cooldown/home. */
static u8 clan_card_request[2048];
static u32 card_request_size,card_request_sequence,card_request_stamp,card_request_echo_crc;
static void write_card_request(void *file,u32 body_hash) {
    if(!card_request_sequence)return;
    u32 h[8]={0x31524347u,card_request_size,card_request_sequence,card_request_stamp,
        body_hash,checksum(clan_card_request,card_request_size),card_request_echo_crc,0};
    h[7]=checksum((const u8*)h,28);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(clan_card_request,1,card_request_size,file)!=card_request_size)
        fail("GuestOffline: local card request checkpoint write failed");
}
static void read_card_request(void *file,u32 body_hash) {
    u32 h[8]={0x31524347u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,28,file)!=28 ||
       h[7]!=checksum((const u8*)h,28) || h[4]!=body_hash ||
       h[1]>sizeof clan_card_request || !h[2] || h[2]>1000000000 || (!h[1] != !h[3]) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(clan_card_request,1,h[1],file)!=h[1] ||
       h[5]!=checksum(clan_card_request,h[1]))
        fail("GuestOffline: invalid local card request checkpoint; original retained");
    card_request_size=h[1];card_request_sequence=h[2];card_request_stamp=h[3];card_request_echo_crc=h[6];
}
