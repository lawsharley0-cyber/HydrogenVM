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
static int initialized,logged_in,battle_started,arena_battle;
static int clan_stream_ready;
static void *ending_battle;
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
static void credit_arena_crowns(void*);
static void friendly_tick(void);
static void friendly_battle_completed(void*);
static void sync_donor(void);
static void validate_donor(void);
static void validate_donor_inventory(void);
static void npc_request_tick(void);
static void encode_donor_member(void*);
static void donation_tick(void);
#ifdef GUEST_PROBE_DONATION_ACTORS
static void donation_actor_probe(void);
#endif

static void fail(const char *why) {
    CALL(API_puts,int,(const char*))(why);
    CALL(API_abort,void,(void))();
}
#include "save.inc"
#include "home_turn.inc"
#include "sector.inc"
#include "support.inc"
static void reply(void*,int,int,const u8*,u32);
static u8 sector_packet[70000];
static u32 prepare_sector_packet(const struct GuestSector *template) {
    static u8 sector[65536];
    u32 size=assemble_sector_template(sector,sizeof sector,template);
    sector_packet[0]=1;for(int i=0;i<4;++i)sector_packet[1+i]=(u8)(size>>(i*8));
    u32 compressed=sizeof sector_packet-5;
    if(CALL(symbol("compress2"),int,(void*,u32*,const void*,u32,int))(sector_packet+5,&compressed,sector,size,6)!=0)
        fail("GuestOffline: native sector compression failed");
    return compressed+5;
}
static void send_training_sector(void *self,int mission) {
    if(mission<0 || mission>=7)fail("GuestOffline: unsupported training sector");
    u32 size=prepare_sector_packet(&npc_sectors[mission]);
    reply(self,21903,1,sector_packet,size);
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
    if(kind==24101) {
        delivered_home=(void*)FIELD(message,0x30);delivered_epoch=saved_epoch;home_clock_logic=0;
        if(!delivered_home)fail("GuestOffline: home clock reply is missing its home");
    }
    if(!CALL(0x188705,int,(void*,void*))((u8*)messaging+0x48,&message)) {
        dispose(message);fail("GuestOffline: native receive queue is full");
    }
    CALL(API_printf,int,(const char*,int))("GuestOffline: queued response=%d\n",kind);
}
#include "reward.inc"
#include "name.inc"
#include "native_actor.inc"
#include "clan_roles.inc"
#include "clan.inc"
#include "donor.inc"
#include "clan_chat.inc"
#include "mail.inc"
#include "card_request.inc"
#include "clan_members.inc"
#include "donation.inc"
#include "npc_request.inc"
#include "match.inc"
#include "friendly.inc"
static void refresh_saved_arena(void) {
    // Recover already-earned saves captured before the native post-setter
    // continuation. The original routine reads the actual NPC threshold,
    // trophy/demotion rules and arena table; no forced arena or win count.
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30),*avatar=(void*)FIELD(owner,0x34);
    if(!home || !avatar)fail("GuestOffline: invalid arena-refresh checkpoint");
    u32 previous=FIELD(avatar,0xac);
    CALL(0x9b709,void,(void*))(avatar);
    if(FIELD(avatar,0xac)!=previous) {
        capture_native(home,avatar);
        CALL(API_puts,int,(const char*))("GuestOffline: persisted native earned-arena refresh");
    }
    dispose(owner);
}
void guest_connect(void *self,void *domain,void *port) {
    (void)domain;(void)port;
    load_guest_home();
    active_messaging=self;
    if(!initialized) {
        struct GuestSinCos zero=guest_sincosf(0.0f);
        if(zero.sine!=0.0f || zero.cosine!=1.0f)fail("GuestOffline: scalar trigonometry unavailable");
        CALL(0x51971,void,(void*,int))(factory,0);initialized=1;
    }
    logged_in=0;battle_started=0;arena_battle=0;ending_battle=0;clan_stream_ready=0;
    delivered_home=0;home_clock_logic=0;
    // A disconnected name reply has no resource transaction to recover.
    // Reloaded native avatar state decides whether an initial request is valid.
    initial_name_pending=0;
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
#ifdef GUEST_PROBE_DONATION_ACTORS
        donation_actor_probe();
#endif
        validate_donor();
        validate_donor_inventory();
        validate_donation_record();
        validate_npc_request();
        // Every extension's wire/semantic validation must precede either
        // friendly-ID migration or seeding a donor into a legacy clan save.
        static u8 checked_mail[33024];encode_local_inbox(checked_mail,sizeof checked_mail);
        validate_clan_checkpoint();
        if(clan_status==2 && !donor_size) {sync_donor();write_save();}
        refresh_saved_arena();
        logged_in=1;
        reply(self,20104,1,initial_login,sizeof initial_login);
        reply(self,24101,0,saved_home,saved_size);
        if(pending_size)reply(self,24111,0,pending_reward,pending_size);
        if(clan_status==1 || clan_status==3 || clan_status==4)
            reply(self,24111,0,clan_command,clan_command_size);
        friendly_resume(self);
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
        local_inbox(self,message);
    } else if(kind==14418 && logged_in && !battle_started) {
        remove_local_mail(self,message);
    } else if(kind==14402 && logged_in) {
        // RoyalTVContentMessage (24405), native VInt entry count: no remote replays.
        const u8 empty=0;reply(self,24405,0,&empty,1);
    } else if(kind==14301 && logged_in && !battle_started) {
        create_clan(self,message);
    } else if(kind==14302 && logged_in && !battle_started) {
        clan_data(self,message);
    } else if(kind==14308 && logged_in && !battle_started) {
        leave_clan(self,message);
    } else if(kind==14316 && logged_in && !battle_started) {
        edit_clan(self,message);
    } else if(kind==14315 && logged_in && !battle_started) {
        chat_to_clan(self,message);
    } else if(kind==14324 && logged_in && !battle_started) {
        search_local_clan(self,message);
    } else if(kind==14304 && logged_in && !battle_started) {
        clan_conversation(self,message);
    } else if(kind==14310 && logged_in && !battle_started) {
        donate_to_npc(self,message);
    } else if(kind==14123 && logged_in && !battle_started) {
        cancel_friendly(self,message);
    } else if(kind==14303 && logged_in && !battle_started) {
        // Original discovery request 0x75ca4 is empty. Native response type
        // getter 0xa1544 is 24304 (not request+10000); its encoder 0xa13a8
        // writes a VInt header count. Local clan creation is a separate port.
        if(CALL(0x198d19,u32,(void*))(message)!=0)
            fail("GuestOffline: invalid clan discovery request");
        const u8 empty=0;reply(self,24304,0,&empty,1);
    } else if((kind==14401 || kind==14403 || kind==14404) && logged_in && !battle_started) {
        // Original encoders: alliance 0x11510 has local/anchor booleans;
        // avatar global/local 0x7017c/0x1af38 have an anchor boolean. Optional
        // anchors are fixed BE LogicLongs. This independent offline instance
        // has no online ranking directory, even when an anchor is requested.
        const u8 *data=(const u8*)FIELD(message,8+0x1c);
        u32 size=CALL(0x198d19,u32,(void*))(message);
        if(size<1)fail("GuestOffline: truncated ranking query");
        u32 mask=kind==14401?3u:1u,anchor=kind==14401?2u:1u;
        if((data[0]&~mask) || size!=((data[0]&anchor)?9u:1u))
            fail("GuestOffline: invalid ranking query extent/flags");
        // All four original response encoders write only a VInt list count:
        // 0x132c6c, 0x33cd0, 0x110294, 0xf9d18. Zero is an actual empty
        // list, not a fabricated player, score, or successful online login.
        int response=kind==14401?((data[0]&1)?24402:24401):kind+10000;
        const u8 empty=0;reply(self,response,0,&empty,1);
    } else if(kind==14406 && logged_in) {
        // BattleReportStreamMessage (24413) echoes its BE LogicLong home ID,
        // followed by a VInt entry count. Replay recording is not ported yet.
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=8)fail("GuestOffline: invalid replay-stream home ID");
        const u8 *data=(const u8*)FIELD(message,8+0x1c);u8 empty[9];
        for(int i=0;i<8;++i)empty[i]=data[i];empty[8]=0;
        reply(self,24413,0,empty,sizeof empty);
    } else if(kind==10212 && logged_in && !battle_started) {
        change_initial_name(self,message);
    } else if((kind==14306 || kind==14307) && logged_in && !battle_started) {
        change_local_member(self,message,kind==14307);
    } else if(kind==14113 && logged_in && !battle_started) {
        // VisitHomeMessage writes a BE LogicLong. Only the human and actual
        // persisted local NPC have profiles; no remote records are invented.
        const u8 *data=(const u8*)FIELD(message,8+0x1c);
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=8)fail("GuestOffline: invalid profile identity extent");
        int human=1,npc=donor_size!=0;
        for(int i=0;i<8;++i) {
            if(data[i]!=(i==7?1:0))human=0;
            if(data[i]!=(i==3?3:i==7?1:0))npc=0;
        }
        if(!human && !npc)fail("GuestOffline: unavailable remote profile identity");
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);
        // VisitedHomeDataMessage::decode 0x39ca8: VInt, native home,
        // native owner avatar, optional visitor-avatar boolean. The existing
        // checkpoint already ends in that false boolean.
        static u8 profile[65537];profile[0]=0;
        const u8 *body=human?saved_home:donor_home;u32 extent=human?saved_size:donor_size;
        for(u32 i=0;i<extent;++i)profile[1+i]=body[i];
        reply(self,24113,0,profile,extent+1);
    } else if(kind==14101 && logged_in) {
        if(friendly_state==2 && battle_started)abandon_friendly();
        battle_started=0;arena_battle=0;reply(self,24101,0,saved_home,saved_size);
    } else if(kind==14104 && logged_in && !battle_started) {
        const u8 *data=(const u8*)FIELD(message,8+0x1c);
        int size=CALL(0x198d19,int,(void*))(message);
        if(size!=2 || data[0]!=18 || data[1]>=7)fail("GuestOffline: unsupported training mission");
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);
        send_training_sector(self,data[1]);battle_started=1;arena_battle=0;
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
        if(consume_home_turn(message))start_arena_match(self);
    } else fail("GuestOffline: unsupported prototype request; progression is not implemented");
    dispose(message);return 1;
}
#ifdef GUEST_PROBE_DONATION_ACTORS
#include "donation_actor_probe.inc"
#endif
