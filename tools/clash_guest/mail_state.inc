/* Local avatar-stream mail survives leaving/disbanding a clan. It is committed
 * with the native home/cooldown, never in a separately durable sidecar. */
static u8 local_mail[32768];
static u32 local_mail_size,local_mail_count,local_mail_sequence,local_mail_echo_crc;
static void write_local_mail(void *file,u32 body_hash) {
    if(!local_mail_sequence)return;
    u32 h[8]={0x31414d47u,local_mail_size,local_mail_count,local_mail_sequence,
              body_hash,checksum(local_mail,local_mail_size),local_mail_echo_crc,0};
    h[7]=checksum((const u8*)h,28);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(local_mail,1,local_mail_size,file)!=local_mail_size)
        fail("GuestOffline: local inbox checkpoint write failed");
}
static void read_local_mail(void *file,u32 body_hash) {
    u32 h[8]={0x31414d47u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,28,file)!=28 ||
       h[7]!=checksum((const u8*)h,28) || h[4]!=body_hash ||
       h[1]>sizeof local_mail || h[2]>32 || !h[3] || h[3]>1000000000 || h[2]>h[3] ||
       (!h[1] != !h[2]) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(local_mail,1,h[1],file)!=h[1] ||
       h[5]!=checksum(local_mail,h[1]))
        fail("GuestOffline: invalid local inbox checkpoint; original retained");
    local_mail_size=h[1];local_mail_count=h[2];local_mail_sequence=h[3];local_mail_echo_crc=h[6];
}
