/* GCL1 belongs to the same atomic file as the native home and clock. A pending
 * creation retains the PRE-command avatar and immutable native command. Crash
 * recovery replays it against that avatar; a completed save contains both the
 * charge/membership and clan metadata. No separate sidecar can lose either. */
static u8 clan_request[1024],clan_command[256];
// Status: 0 disbanded (serial retained), 1 create pending, 2 active,
// 3 leave pending, 4 settings pending. Pending commands always apply to the
// pre-command native avatar in this same file, never a separately saved one.
static u32 clan_serial,clan_status,clan_request_size,clan_command_size;
static u32 clan_expects_donor;
static int clan_gold_before,clan_gold_after;
static u8 clan_chat[32768];
static u32 clan_chat_size,clan_chat_count,clan_chat_sequence;
static int clan_capture_ready(void*);
static void write_clan(void *file,u32 body_hash) {
    if(!clan_serial)return;
    u32 header[10]={0x314c4347u,clan_serial,clan_status|(clan_expects_donor?0x100u:0),clan_request_size,
        clan_command_size,(u32)clan_gold_before,(u32)clan_gold_after,body_hash,
        checksum(clan_request,clan_request_size)^checksum(clan_command,clan_command_size),0};
    header[9]=checksum((const u8*)header,36);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(header,1,sizeof header,file)!=sizeof header ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(clan_request,1,clan_request_size,file)!=clan_request_size ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(clan_command,1,clan_command_size,file)!=clan_command_size)
        fail("GuestOffline: clan transaction write failed");
}
static void read_clan(void *file,u32 body_hash) {
    u32 h[10]={0x314c4347u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,36,file)!=36 ||
       h[9]!=checksum((const u8*)h,36) || h[7]!=body_hash ||
       !h[1] || h[1]>1000000 || (h[2]&~0x1ffu) || (h[2]&255u)>4 ||
       ((h[2]&0x100u) && (h[2]&255u)<2) ||
       h[3]>sizeof clan_request || h[4]>sizeof clan_command ||
       ((h[2]&255u) && (h[3]<10 || h[4]<16)) || (!(h[2]&255u) && (h[3] || h[4])) ||
       h[5]>2000000000u || h[6]>h[5])
        fail("GuestOffline: invalid clan transaction header; original retained");
    if(CALL(s_read,u32,(void*,u32,u32,void*))(clan_request,1,h[3],file)!=h[3] ||
       CALL(s_read,u32,(void*,u32,u32,void*))(clan_command,1,h[4],file)!=h[4] ||
       h[8]!=(checksum(clan_request,h[3])^checksum(clan_command,h[4])))
        fail("GuestOffline: invalid clan transaction body; original retained");
    clan_serial=h[1];clan_status=h[2]&255u;clan_expects_donor=(h[2]>>8)&1u;
    clan_request_size=h[3];clan_command_size=h[4];
    clan_gold_before=(int)h[5];clan_gold_after=(int)h[6];
}
static void write_clan_chat(void *file,u32 body_hash) {
    if(!clan_chat_size)return;
    u32 h[7]={0x31534347u,clan_chat_size,clan_chat_count,clan_chat_sequence,
              body_hash,checksum(clan_chat,clan_chat_size),0};
    h[6]=checksum((const u8*)h,24);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(clan_chat,1,clan_chat_size,file)!=clan_chat_size)
        fail("GuestOffline: clan conversation write failed");
}
static void read_clan_chat(void *file,u32 body_hash) {
    u32 h[7]={0x31534347u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,24,file)!=24 ||
       h[6]!=checksum((const u8*)h,24) || h[4]!=body_hash ||
       h[1]<8 || h[1]>sizeof clan_chat || !h[2] || h[2]>64 || h[3]<h[2] || h[3]>1000000000 ||
       CALL(s_read,u32,(void*,u32,u32,void*))(clan_chat,1,h[1],file)!=h[1] ||
       h[5]!=checksum(clan_chat,h[1]))
        fail("GuestOffline: invalid clan conversation; original retained");
    clan_chat_size=h[1];clan_chat_count=h[2];clan_chat_sequence=h[3];
}
