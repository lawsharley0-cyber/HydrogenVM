/* Bounded, persistent local conversation. Native ChatStreamEntry encodes the
 * actual author/name/role and text. No simulated person claims to be online. */
#include "clan_chat_wire.inc"
#include "card_request_wire.inc"
static u32 encode_clan_conversation(u8 *output,u32 capacity) {
    struct SectorWriter w={output,0,capacity};
    sector_vint(&w,(int)clan_chat_count+(card_request_size?1:0));
    struct GuestReader r={clan_chat,clan_chat_size,0,0,0};u32 now=wall_epoch();
    int request_written=0;
    for(u32 i=0;i<clan_chat_count;++i) {
        u32 stamp=turn_int(&r),size=turn_int(&r);
        if(size<16 || size>1024 || size>r.size-r.at)fail("GuestOffline: invalid conversation record boundary");
        if(card_request_size && !request_written && stamp>=card_request_stamp) {
            card_request_entry(&w,clan_card_request,card_request_size,card_request_stamp,now);request_written=1;
        }
        clan_chat_entry(&w,r.data+r.at,size,stamp,now);r.at+=size;
    }
    if(r.at!=r.size)fail("GuestOffline: trailing conversation records");
    if(card_request_size && !request_written)
        card_request_entry(&w,clan_card_request,card_request_size,card_request_stamp,now);
    return w.size;
}
static void send_clan_conversation(void *self) {
    if(clan_status!=2)fail("GuestOffline: conversation requested without completed membership");
    static u8 output[35136];u32 size=encode_clan_conversation(output,sizeof output);
    reply(self,24311,0,output,size);
}
static void clan_conversation(void *self,void *message) {
    if(CALL(0x198d19,u32,(void*))(message)!=0)fail("GuestOffline: invalid local conversation request");
    send_clan_conversation(self);
}
static void chat_to_clan(void *self,void *message) {
    if(clan_status!=2 || pending_size)fail("GuestOffline: clan chat during an incomplete transaction");
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader r={data,size,0,0,0};clan_skip_string(&r,512,1);
    if(r.at!=r.size || !FIELD(message,0x30))fail("GuestOffline: invalid clan chat request");
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    void *owner=decode_packet(24101,0,saved_home,saved_size),*avatar=(void*)FIELD(owner,0x34);
    if(!avatar || clan_chat_sequence>=1000000000)fail("GuestOffline: conversation author/sequence unavailable");
    u8 encoded[1024];struct SectorWriter w={encoded,0,sizeof encoded};
    u32 stream[16],entry[10]={0},identity[2]={0,++clan_chat_sequence};
    CALL(0x192a71,void,(void*,u32))(stream,1024);
    CALL(0x193749,void,(void*,int))(stream,2);
    FIELD(entry,4)=(u32)identity;FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(avatar);
    FIELD(entry,0xc)=FIELD(entry,8);FIELD(entry,0x10)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(avatar);FIELD(entry,0x18)=2;
    FIELD(entry,0x24)=FIELD(message,0x30);
    CALL(0x11ca71,void,(void*,void*))(entry,stream);
    u32 length=CALL(0x192b2d,u32,(void*))(stream);
    const u8 *bytes=CALL(0x1936fd,const u8*,(void*))(stream);u32 now=wall_epoch();
    // Independently bounds-check the exact encoded record before persistence.
    clan_chat_entry(&w,bytes,length,now,now);
    while(clan_chat_count>=64 || w.size+8>sizeof clan_chat-clan_chat_size) {
        struct GuestReader old={clan_chat,clan_chat_size,0,0,0};turn_int(&old);u32 drop=turn_int(&old);
        if(!clan_chat_count || drop>old.size-old.at)fail("GuestOffline: invalid conversation eviction boundary");
        drop+=8;for(u32 i=drop;i<clan_chat_size;++i)clan_chat[i-drop]=clan_chat[i];
        clan_chat_size-=drop;--clan_chat_count;
    }
    struct SectorWriter saved={clan_chat,clan_chat_size,sizeof clan_chat};
    clan_int(&saved,now);clan_int(&saved,w.size);sector_bytes(&saved,encoded,w.size);
    clan_chat_size=saved.size;++clan_chat_count;write_save();
    reply(self,24312,0,encoded,w.size);
    CALL(0x19370d,void,(void*))(stream);dispose(owner);
    CALL(API_puts,int,(const char*))("GuestOffline: native local chat entry committed and delivered");
}
static void validate_clan_checkpoint(void) {
    // Validate all durable extensions before refresh/timer snapshots can write
    // the loaded home. Invalid state fails with the original file untouched.
    if((clan_status==1 || clan_status==3 || clan_status==4) && pending_size)
        fail("GuestOffline: overlapping durable clan/reward transactions");
    void *owner=decode_packet(24101,0,saved_home,saved_size),*avatar=(void*)FIELD(owner,0x34);
    if(!avatar)fail("GuestOffline: clan checkpoint has no native avatar");
    void *id=CALL(0x9ae01,void*,(void*))(avatar);
    if(!clan_status) {
        if(id || clan_chat_size || card_request_size)fail("GuestOffline: clan metadata is missing for native membership/conversation");
        dispose(owner);return;
    }
    struct ClanFields f=clan_fields(clan_request,clan_request_size);
    struct GuestReader r={clan_command,clan_command_size,0,0,0};int kind=turn_vint(&r);
    if((clan_status==1 && kind!=209) || (clan_status==3 && kind!=208) ||
       (clan_status==4 && kind!=212) || (clan_status==2 && kind!=209 && kind!=212))
        fail("GuestOffline: journal command differs from clan operation");
    u8 expected[256];struct SectorWriter w={expected,0,sizeof expected};
    sector_vint(&w,kind);clan_id(&w);
    if(kind==209)clan_string(&w,clan_request+f.name_at,f.name_size);
    if(kind!=208){sector_vint(&w,16);sector_vint(&w,f.badge);}
    if(kind!=212)sector_byte(&w,kind==209);
    clan_server_metadata(&w);
    if(w.size!=clan_command_size)fail("GuestOffline: invalid clan journal command extent");
    for(u32 i=0;i<w.size;++i)if(expected[i]!=clan_command[i])
        fail("GuestOffline: immutable clan command differs from metadata");
    if(clan_status==1) {
        if(id || CALL(0x9ab55,int,(void*))(avatar)!=clan_gold_before || clan_chat_size ||
           clan_gold_before-clan_gold_after!=1000)
            fail("GuestOffline: pending clan journal has no original pre-charge avatar");
    } else {
        if(!id || FIELD(id,0)!=1 || FIELD(id,4)!=clan_serial || CALL(0x9ae41,int,(void*))(avatar)!=2)
            fail("GuestOffline: saved native membership differs from clan metadata");
        check_native_clan_name(avatar);
        if(clan_status==2) {
            void *badge=CALL(0x9ae39,void*,(void*))(avatar);
            if(!badge || CALL(0x7d2e1,int,(void*))(badge)!=16000000+f.badge)
                fail("GuestOffline: saved native clan badge differs from metadata");
        }
    }
    dispose(owner);
    static u8 checked[35136];encode_clan_conversation(checked,sizeof checked);
}
