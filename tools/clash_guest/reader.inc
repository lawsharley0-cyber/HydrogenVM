/* Bounds-checked pinned ByteStream primitives shared by turns and snapshots. */
struct GuestReader {const u8 *data;u32 size,at;int bits;u8 packed;};
static u8 turn_byte(struct GuestReader *r) {
    r->bits=0;if(r->at>=r->size)fail("GuestOffline: truncated native stream");
    return r->data[r->at++];
}
static int turn_vint(struct GuestReader *r) {
    u8 first=turn_byte(r),b=first;u32 value=first&63,shift=6;
    for(int i=1;b&128;++i) {
        b=turn_byte(r);if(i>4 || (i==4 && b>15))fail("GuestOffline: invalid native VInt");
        value|=(u32)(b&127)<<shift;shift+=7;
    }
    if(first&64)value|=shift<32?~0u<<shift:0x80000000u;
    return (int)value;
}
static u32 turn_int(struct GuestReader *r) {
    u32 value=0;for(int i=0;i<4;++i)value=(value<<8)|turn_byte(r);
    return value;
}
static int turn_bool(struct GuestReader *r) {
    if(!r->bits){r->packed=turn_byte(r);r->bits=8;}
    int value=r->packed&1;r->packed>>=1;--r->bits;return value;
}
static void turn_reference(struct GuestReader *r) {
    int type=turn_vint(r);if(!type)return;
    int index=turn_vint(r);
    if(type<1 || type>999 || index<0 || index>=1000000)fail("GuestOffline: invalid data reference");
}
