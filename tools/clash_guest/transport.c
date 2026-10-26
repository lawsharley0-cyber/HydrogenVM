/* ARMv7 guest-local experimental transport for the pinned client.
 * Build-time function replacements, not emulator callbacks or runtime JIT.
 * Training prototype: see GUEST_MOD_PROGRESS.md for actual qualifications.
 * Remaining economy/arena paths must be ported before replacing the host backend.
 */
#include "generated.h"
typedef unsigned int u32;
typedef unsigned char u8;
#define CALL(address, result, args) ((result (*)args)(address))
#define FIELD(p, offset) (*(u32 *)((u8 *)(p)+(offset)))
#define BYTE(p, offset) (*((u8 *)(p)+(offset)))
// ARM aggregate copies/initialization may lower to these even in freestanding
// code. Resolve through existing pinned imports; add no dynamic dependencies.
void *memcpy(void *dest,const void *source,u32 size) {
    return CALL(API_memcpy,void*,(void*,const void*,u32))(dest,source,size);
}
void *memset(void *dest,int value,u32 size) {
    return CALL(API_memset,void*,(void*,int,u32))(dest,value,size);
}
static u32 factory[4];
static int initialized,logged_in,battle_started;
static void *active_messaging;
static u8 pending_reward[65536];
static u32 pending_size;
static int pending_kind,pending_id,reward_applied,requested_kind,requested_id;
static int completed_kind,completed_id;
struct GuestSinCos {float sine,cosine;};
struct GuestSinCos guest_sincosf(float angle) {
    // This pair returns through the compiler-generated ARMv7 hidden pointer.
    // Use ordinary scalar APIs so older hosts' incorrect pair-return shim is
    // no longer on the turret-position path. Do not freeze the native recoil.
    struct GuestSinCos value;
    value.sine=CALL(API_sinf,float,(float))(angle);
    value.cosine=CALL(API_cosf,float,(float))(angle);
    return value;
}
static void *decode_packet(int,int,const u8*,u32);
static void dispose(void*);
static void prepare_reward(void*,void*);

static void fail(const char *why) {
    CALL(API_puts,int,(const char*))(why);
    CALL(API_abort,void,(void))();
}
#include "save.inc"
#include "home_turn.inc"
#include "sector.inc"
#include "support.inc"
static void reply(void*,int,int,const u8*,u32);
static void send_training_sector(void *self,int mission) {
    static u8 sector[65536],packet[70000];
    u32 size=assemble_sector(sector,sizeof sector,mission);
    packet[0]=1;for(int i=0;i<4;++i)packet[1+i]=(u8)(size>>(i*8));
    u32 compressed=sizeof packet-5;
    if(CALL(symbol("compress2"),int,(void*,u32*,const void*,u32,int))(packet+5,&compressed,sector,size,6)!=0)
        fail("GuestOffline: native sector compression failed");
    reply(self,21903,1,packet,compressed+5);
}
static void dispose(void *message) {
    u32 *vtable=(u32 *)FIELD(message,0);
    CALL(vtable[6],void,(void*))(message); // native destruct, then deleting destructor
    CALL(vtable[1],void,(void*))(message);
}
static void *decode_packet(int kind,int version,const u8 *bytes,u32 size) {
    void *message=CALL(0x5199d,void*,(void*,int))(factory,kind);
    if(!message)fail("GuestOffline: original message factory rejected response");
    void *data=CALL(API_malloc,void*,(u32))(size?size:1);
    if(!data)fail("GuestOffline: response allocation failed");
    for(u32 i=0;i<size;++i)((u8*)data)[i]=bytes[i];
    FIELD(message,4)=version;
    CALL(0x1936d5,void,(void*,void*,u32))((u8*)message+8,data,size);
    u32 *vtable=(u32 *)FIELD(message,0);
    CALL(vtable[3],void,(void*))(message); // original pinned message decoder
    return message;
}
static void reply(void *messaging,int kind,int version,const u8 *bytes,u32 size) {
    void *message=decode_packet(kind,version,bytes,size);
    if(!CALL(0x188705,int,(void*,void*))((u8*)messaging+0x48,&message)) {
        dispose(message);fail("GuestOffline: native receive queue is full");
    }
    CALL(API_printf,int,(const char*,int))("GuestOffline: queued response=%d\n",kind);
}
#include "reward.inc"
void guest_connect(void *self,void *domain,void *port) {
    (void)domain;(void)port;
    load_guest_home();
    active_messaging=self;
    if(!initialized) {
        struct GuestSinCos zero=guest_sincosf(0.0f);
        if(zero.sine!=0.0f || zero.cosine!=1.0f)fail("GuestOffline: scalar trigonometry unavailable");
        CALL(0x51971,void,(void*,int))(factory,0);initialized=1;
    }
    logged_in=0;battle_started=0;
    BYTE(self,0xe4)=1; // Connection::isConnected; this transport uses a local queue
    BYTE(self,0x108)=1;BYTE(self,0x109)=0;BYTE(self,0x10a)=0;
    CALL(0x187809,void,(void*,void*))(self,(u8*)self+0xe0);
    CALL(API_puts,int,(const char*))("GuestOffline: ARMv7 local transport ready (prototype)");
}
void guest_disconnect(void *self) {
    CALL(0x187241,void,(void*))(self); // original queue drain releases queued objects
    BYTE(self,0xe4)=0;
    BYTE(self,0x109)=0;BYTE(self,0x10a)=0;BYTE(self,0x10b)=0;
    BYTE(self,0x130)=0;BYTE(self,0x131)=0;
    logged_in=0;battle_started=0;
    CALL(API_puts,int,(const char*))("GuestOffline: local transport disconnected");
}
int guest_send(void *self,void *message) {
    if(!BYTE(self,0xe4))fail("GuestOffline: send while disconnected");
    u32 *vtable=(u32 *)FIELD(message,0);
    int kind=CALL(vtable[5],int,(void*))(message);
    CALL(vtable[2],void,(void*))(message);
    CALL(API_printf,int,(const char*,int))("GuestOffline: outgoing kind=%d\n",kind);
    if(kind==10101) {
        if(logged_in)fail("GuestOffline: duplicate login");
        logged_in=1;
        reply(self,20104,1,initial_login,sizeof initial_login);
        reply(self,24101,0,saved_home,saved_size);
        if(pending_size)reply(self,24111,0,pending_reward,pending_size);
    } else if(kind==10108) {
        // Native MessageManager can send this while CSV loading delays login.
        reply(self,20108,0,0,0);
    } else if((kind==10512 || kind==10513) && logged_in) {
        // FriendListMessage decoder 0x716bc reads two BE ints. Native
        // MessageManager routes type 1 to Facebook, type 2 to Game Center.
        // This independent offline profile has no external friend directory.
        u8 empty_list[8]={0,0,0,kind==10513?1:2,0,0,0,0};
        reply(self,20105,0,empty_list,sizeof empty_list);
    } else if(kind==14405 && logged_in) {
        // AvatarStreamMessage (24411), native decoder 0xf93d0: VInt count.
        const u8 empty=0;reply(self,24411,0,&empty,1);
    } else if(kind==14402 && logged_in) {
        // RoyalTVContentMessage (24405), native VInt entry count: no remote replays.
        const u8 empty=0;reply(self,24405,0,&empty,1);
    } else if(kind==14406 && logged_in) {
        // BattleReportStreamMessage (24413) echoes its BE LogicLong home ID,
        // followed by a VInt entry count. Replay recording is not ported yet.
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=8)fail("GuestOffline: invalid replay-stream home ID");
        const u8 *data=(const u8*)FIELD(message,8+0x1c);u8 empty[9];
        for(int i=0;i<8;++i)empty[i]=data[i];empty[8]=0;
        reply(self,24413,0,empty,sizeof empty);
    } else if(kind==14113 && logged_in && !battle_started) {
        // VisitHomeMessage writes a BE LogicLong. This independent profile
        // only has its own local home; do not invent remote player records.
        const u8 *data=(const u8*)FIELD(message,8+0x1c);
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=8)fail("GuestOffline: invalid profile identity extent");
        for(int i=0;i<8;++i)if(data[i]!=(i==7?1:0))fail("GuestOffline: unavailable remote profile identity");
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);
        // VisitedHomeDataMessage::decode 0x39ca8: VInt, native home,
        // native owner avatar, optional visitor-avatar boolean. The existing
        // checkpoint already ends in that false boolean.
        static u8 profile[65537];profile[0]=0;
        for(u32 i=0;i<saved_size;++i)profile[1+i]=saved_home[i];
        reply(self,24113,0,profile,saved_size+1);
    } else if(kind==14101 && logged_in) {
        battle_started=0;reply(self,24101,0,saved_home,saved_size);
    } else if(kind==14104 && logged_in && !battle_started) {
        const u8 *data=(const u8*)FIELD(message,8+0x1c);
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=2 || data[0]!=18 || data[1]>=7)fail("GuestOffline: unsupported training mission");
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);
        send_training_sector(self,data[1]);battle_started=1;
    } else if(kind==10107 && logged_in) {
        // Original ClientCapabilitiesMessage (getMessageType 0xbd10) is
        // one-way metadata and has no server response.
    } else if(kind==12951 && logged_in && battle_started) {
        // BattleScreen::sendBattleEvent sends the opponent-facing UI event
        // separately from the native command stream. An offline opponent has
        // no remote UI to notify; do not replay it as an authoritative command.
        void *event=(void*)FIELD(message,0x30);
        if(!event)fail("GuestOffline: absent battle UI event");
        int type=CALL(0x32071,int,(void*))(event);
        int length=CALL(0x198d19,int,(void*))(message);
        if(type<0 || type>3 || length<1 || length>4096)
            fail("GuestOffline: unsupported battle UI event");
        CALL(API_printf,int,(const char*,int))("GuestOffline: consumed local battle UI event=%d\n",type);
    } else if(kind==14102 && logged_in && battle_started) {
        // Native NPC1 simulation executes its own battle commands. No invented
        // checksum acknowledgements or duplicated server-side commands.
    } else if(kind==14102 && logged_in) {
        consume_home_turn(message);
    } else fail("GuestOffline: unsupported prototype request; progression is not implemented");
    dispose(message);return 1;
}
