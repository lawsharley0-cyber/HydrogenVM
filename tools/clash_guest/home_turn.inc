/* Exact native home echo schemas already decoded by ClashState.cpp. These
 * commands execute locally before EndClientTurn; persist that native state.
 * Reward/matchmaking commands remain unsupported until their guest port exists.
 */
#include "reader.inc"
static void name_echo(struct GuestReader*);
static void clan_echo(struct GuestReader*,int);
static void mail_echo(struct GuestReader*);
static void reward_echo(struct GuestReader *r) {
    // A successfully executed server command is echoed by the native client.
    // Its executor is the server's (0,0), not the client's (0,1). Decode it
    // with the original class and qualify its source; never execute it again.
    u32 remaining=r->size-r->at,stream[16],command[11];
    void *data=CALL(API_malloc,void*,(u32))(remaining);
    if(!data)fail("GuestOffline: echo allocation failed");
    for(u32 i=0;i<remaining;++i)((u8*)data)[i]=r->data[r->at+i];
    CALL(0x192a71,void,(void*,u32))(stream,remaining);
    CALL(0x1936d5,void,(void*,void*,u32))(stream,data,remaining);
    CALL(0x81fed,void,(void*))(command);
    CALL(0x828ad,void,(void*,void*))(command,stream);
    u32 used=CALL(0x192b45,u32,(void*))(stream); // getOffset, not the full buffer length
    int kind=(int)FIELD(command,0x24),id=(int)FIELD(command,0x20);
    int valid=(pending_size && reward_applied && kind==pending_kind && id==pending_id) ||
              (completed_kind && kind==completed_kind && id==completed_id);
    CALL(0x82011,void,(void*))(command);
    CALL(0x19370d,void,(void*))(stream);
    if(!valid || used>remaining || remaining-used<4)fail("GuestOffline: unexpected reward echo");
    r->at+=used;r->bits=0;
}
static int consume_home_turn(void *message) {
    struct GuestReader r={(const u8*)FIELD(message,8+0x1c),
                          CALL(0x198d19,u32,(void*))(message),0,0,0};
    if(turn_vint(&r)<0)fail("GuestOffline: negative home tick");
    turn_vint(&r);int count=turn_vint(&r);
    if(count<0 || count>64)fail("GuestOffline: invalid home command count");
    int matchmaking=0;
    for(int i=0;i<count;++i) {
        int kind=turn_vint(&r);
        if(kind==213){reward_echo(&r);continue;}
        if(kind==201){name_echo(&r);continue;}
        if(kind==208 || kind==209 || kind==212){clan_echo(&r,kind);continue;}
        if(kind==533){mail_echo(&r);continue;}
        turn_vint(&r);turn_vint(&r);
        int high=turn_vint(&r),low=turn_vint(&r);
        if(high!=0 || low!=1)fail("GuestOffline: home command identity mismatch");
        if(kind==500){turn_vint(&r);turn_vint(&r);turn_bool(&r);turn_bool(&r);}
        else if(kind==504)turn_reference(&r);
        else if(kind==525) {
            int spells=turn_vint(&r);if(spells<0 || spells>=200)fail("GuestOffline: invalid spell list");
            for(int j=0;j<spells;++j)turn_reference(&r);
        } else if(kind==522) {
            // LogicClearChestSourceCommand clears the native "new" badge.
            if(turn_vint(&r)<0)fail("GuestOffline: invalid chest badge identity");
        } else if(kind==502) {
            // Original LogicStartExploringCommand starts the native timer;
            // it neither grants a reward nor acknowledges a server command.
            if(turn_vint(&r)<0)fail("GuestOffline: invalid unlock chest identity");
        } else if(kind==503 || kind==506) {
            int id=turn_vint(&r);if(id<0)fail("GuestOffline: invalid claim chest identity");
            if(kind==506)turn_bool(&r);
            requested_kind=1;requested_id=id;
        } else if(kind==518 || kind==523) {
            requested_kind=kind==518?2:3;requested_id=0;
        } else if(kind==537) {
            // Native decode 0xbbf4c reads a VInt and packed boolean.
            // The actual Arena-1 Battle control sends selector 0, flag false.
            // Queue nothing until the entire command bundle/trailer validates.
            int selector=turn_vint(&r),flag=turn_bool(&r);
            CALL(API_printf,int,(const char*,int,int,u32))(
                "GuestOffline: arena request selector=%d flag=%d remaining=%u\n",selector,flag,r.size-r.at);
            if(selector!=0 || flag || matchmaking)fail("GuestOffline: unsupported matchmaking variant");
            matchmaking=1;
        } else if(kind==539) {
            // LogicPageOpenedCommand::decode 0xd8af8 reads one VInt;
            // execute 0xd8b10 accepts native page IDs 1 through 4.
            int page=turn_vint(&r);
            if(page<1 || page>4)fail("GuestOffline: invalid opened page");
        } else if(kind==516 || kind==538) {
            // LogicUpdateLastShownLevelUpCommand (516), encode/decode
            // 0x1109bc/0x1109c0 tail-call LogicCommand with no extra payload.
            // Its original execute 0x1109c4 updates lastShownLevelUp; retain
            // that native home state instead of aborting on the next launch.
        } else {
            CALL(API_printf,int,(const char*,int))("GuestOffline: unported home command=%d\n",kind);
            fail("GuestOffline: reward/matchmaking command requires guest implementation");
        }
    }
    for(int i=0;i<4;++i)if(turn_byte(&r)!=255)fail("GuestOffline: unsupported home debug trailer");
    if(r.at!=r.size)fail("GuestOffline: trailing home turn data");
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    return matchmaking;
}
