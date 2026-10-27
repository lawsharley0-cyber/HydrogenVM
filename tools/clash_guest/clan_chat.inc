/* Bounded, persistent local conversation. Native ChatStreamEntry encodes the
 * actual author/name/role and text. No simulated person claims to be online. */
#include "clan_chat_wire.inc"
#include "card_request_wire.inc"
#include "friendly_wire.inc"
#include "friendly_wallet.inc"
static void friendly_migrate_identity(void);
static void conversation_pending(struct SectorWriter *w,u32 now,u32 before,u32 *written) {
    for(;;) {
        int choice=-1;u32 stamp=0xffffffffu;
        if(friendly_sequence && !(*written&1) && friendly_stamp<=before) {choice=0;stamp=friendly_stamp;}
        if(card_request_size && !(*written&2) && card_request_stamp<=before &&
           (choice<0 || card_request_stamp<stamp)) {choice=1;stamp=card_request_stamp;}
        if(npc_request_size && !(*written&4) && npc_request_stamp<=before &&
           (choice<0 || npc_request_stamp<stamp)) {choice=2;stamp=npc_request_stamp;}
        if(choice<0)return;
        if(choice==0)friendly_entry_wire(w,friendly_state==3?friendly_result:friendly_entry,
            friendly_state==3?friendly_result_size:friendly_entry_size,friendly_stamp,now);
        if(choice==1)card_request_entry(w,clan_card_request,card_request_size,card_request_stamp,now);
        if(choice==2)npc_request_entry(w,npc_request,npc_request_size,npc_request_stamp,now,npc_request_sequence);
        *written|=1u<<choice;
    }
}
static u32 encode_clan_conversation(u8 *output,u32 capacity) {
    struct SectorWriter w={output,0,capacity};
    sector_vint(&w,(int)clan_chat_count+(card_request_size?1:0)+(friendly_sequence?1:0)+(npc_request_size?1:0));
    struct GuestReader r={clan_chat,clan_chat_size,0,0,0};u32 now=wall_epoch(),written=0;
    for(u32 i=0;i<clan_chat_count;++i) {
        u32 stamp=turn_int(&r),size=turn_int(&r);
        if(size<16 || size>1024 || size>r.size-r.at)fail("GuestOffline: invalid conversation record boundary");
        conversation_pending(&w,now,stamp,&written);
        clan_chat_entry(&w,r.data+r.at,size,stamp,now);r.at+=size;
    }
    if(r.at!=r.size)fail("GuestOffline: trailing conversation records");
    conversation_pending(&w,now,0xffffffffu,&written);
    return w.size;
}
static void send_clan_conversation(void *self) {
    if(clan_status!=2)fail("GuestOffline: conversation requested without completed membership");
    static u8 output[70000];u32 size=encode_clan_conversation(output,sizeof output);
    reply(self,24311,0,output,size);
    clan_stream_ready=1;
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
    FIELD(entry,0x14)=(u32)CALL(0x9abcd,int,(void*))(avatar);FIELD(entry,0x18)=(u32)CALL(0x9ae41,int,(void*))(avatar);
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
        if(id || clan_chat_size || card_request_size || friendly_sequence)fail("GuestOffline: clan metadata is missing for native membership/conversation");
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
        if(!id || FIELD(id,0)!=1 || FIELD(id,4)!=clan_serial ||
           !clan_roles_valid(CALL(0x9ae41,int,(void*))(avatar),(int)donor_present,donor_role))
            fail("GuestOffline: saved native membership differs from clan metadata");
        check_native_clan_name(avatar);
        if(clan_status==2) {
            void *badge=CALL(0x9ae39,void*,(void*))(avatar);
            if(!badge || CALL(0x7d2e1,int,(void*))(badge)!=16000000+f.badge)
                fail("GuestOffline: saved native clan badge differs from metadata");
        }
    }
    if(friendly_sequence) {
        if(!friendly_wallet_valid(friendly_state,(int)clan_status,
           CALL(0x9ab55,int,(void*))(avatar),CALL(0x9ab7d,int,(void*))(avatar),
           friendly_gold_before,friendly_gold_after,friendly_free_before,friendly_free_after))
            fail("GuestOffline: saved friendly wallet differs from native avatar");
    }
    dispose(owner);
    static u8 checked[70000];encode_clan_conversation(checked,sizeof checked);
    friendly_migrate_identity();
}
