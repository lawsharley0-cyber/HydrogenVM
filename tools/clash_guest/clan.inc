/* Original 2016 clan messages/encoders. This directory contains only the
 * player's actual local clan. There is no remote player or online service. */
#include "clan_wire.inc"
static void send_clan_conversation(void*);
static void clan_int(struct SectorWriter *w,u32 value) {
    for(int shift=24;shift>=0;shift-=8)sector_byte(w,(u8)(value>>shift));
}
static void clan_id(struct SectorWriter *w) {clan_int(w,1);clan_int(w,clan_serial);}
static void clan_string(struct SectorWriter *w,const u8 *data,u32 size) {
    clan_int(w,size);sector_bytes(w,data,size);
}
static void check_native_clan_name(void *avatar) {
    struct ClanFields f=clan_fields(clan_request,clan_request_size);
    void *name=CALL(0x9ae31,void*,(void*))(avatar);
    if(!name || CALL(0x18f4bd,u32,(void*))(name)!=f.name_size)
        fail("GuestOffline: native clan name extent differs from metadata");
    const u8 *bytes=CALL(0x19c099,const u8*,(void*))(name);
    if(!bytes)fail("GuestOffline: native clan name bytes unavailable");
    for(u32 i=0;i<f.name_size;++i)if(bytes[i]!=clan_request[f.name_at+i])
        fail("GuestOffline: native clan name differs from metadata");
}
static int clan_capture_ready(void *avatar) {
    void *id=CALL(0x9ae01,void*,(void*))(avatar);
    if(!clan_status) {
        if(id)fail("GuestOffline: native membership has no local clan metadata");
        return 1;
    }
    if(clan_status==3) {
        if(id)return 0;
        clan_status=0;clan_request_size=0;clan_command_size=0;
        clan_chat_size=0;clan_chat_count=0;clan_chat_sequence=0;
        card_request_size=0;card_request_stamp=0;
        CALL(API_puts,int,(const char*))("GuestOffline: original clan leave applied; committing disbanded solo clan");
        return 1;
    }
    if(clan_status==1 && !id) {
        if(CALL(0x9ab55,int,(void*))(avatar)!=clan_gold_before)
            fail("GuestOffline: pending clan creation wallet changed");
        return 0;
    }
    if(!id || FIELD(id,0)!=1 || FIELD(id,4)!=clan_serial || CALL(0x9ae41,int,(void*))(avatar)!=2)
        fail("GuestOffline: native clan membership differs from committed transaction");
    check_native_clan_name(avatar);
    if(clan_status==1) {
        if(CALL(0x9ab55,int,(void*))(avatar)!=clan_gold_after)
            fail("GuestOffline: original clan creation charge mismatch");
        clan_status=2;
        CALL(API_puts,int,(const char*))("GuestOffline: original clan creation applied; committing native membership and charge");
    }
    if(clan_status==4) {
        struct ClanFields f=clan_fields(clan_request,clan_request_size);
        void *badge=CALL(0x9ae39,void*,(void*))(avatar);
        if(!badge || CALL(0x7d2e1,int,(void*))(badge)!=16000000+f.badge)return 0;
        clan_status=2;
    }
    return 1;
}
static void create_clan(void *self,void *message) {
    if(clan_status || pending_size || initial_name_pending)
        fail("GuestOffline: clan creation during another transaction/membership");
    const u8 *data=(const u8*)FIELD(message,8+0x1c);
    u32 size=CALL(0x198d19,u32,(void*))(message);
    struct ClanFields f=clan_fields(data,size);
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    void *owner=decode_packet(24101,0,saved_home,saved_size),*avatar=(void*)FIELD(owner,0x34);
    void *globals=CALL(0xb81b9,void*,(void))();
    int cost=CALL(0x68b55,int,(void*))(globals);
    void *resource=CALL(0x68b51,void*,(void*))(globals);
    if(!avatar || CALL(0x9ae1d,int,(void*))(avatar) ||
       CALL(0x9abcd,int,(void*))(avatar)<3 || resource!=CALL(0xb8309,void*,(void))() || cost!=1000)
        fail("GuestOffline: original clan creation gate/configuration mismatch");
    clan_gold_before=CALL(0x9ab55,int,(void*))(avatar);
    if(clan_gold_before<cost)fail("GuestOffline: insufficient native clan creation resource");
    clan_gold_after=clan_gold_before-cost;
    if(clan_serial>=1000000)fail("GuestOffline: local clan identity exhausted");
    ++clan_serial;clan_request_size=size;
    for(u32 i=0;i<size;++i)clan_request[i]=data[i];
    struct SectorWriter w={clan_command,0,sizeof clan_command};
    sector_vint(&w,209);clan_id(&w);clan_string(&w,data+f.name_at,f.name_size);
    sector_vint(&w,16);sector_vint(&w,f.badge);sector_byte(&w,1);
    // Original LogicServerCommand default ID -1; LogicCommand tick/sequence
    // default -1, executor (0,0). Preserve native server-command semantics.
    const int metadata[]={-1,-1,-1,0,0};
    for(int i=0;i<5;++i)sector_vint(&w,metadata[i]);
    clan_command_size=w.size;clan_status=1;write_save();dispose(owner);
    CALL(API_puts,int,(const char*))("GuestOffline: clan creation journal committed before native delivery");
    reply(self,24111,0,clan_command,clan_command_size);
}
static void encode_clan_header(void *stream,void *request,void *avatar) {
    struct ClanFields f=clan_fields(clan_request,clan_request_size);
    u32 header[15]={0},id[2]={1,clan_serial};
    FIELD(header,0)=(u32)id;FIELD(header,4)=FIELD(request,0x30);FIELD(header,8)=FIELD(request,0x38);
    FIELD(header,0xc)=(u32)f.type;FIELD(header,0x10)=1;
    FIELD(header,0x14)=FIELD(avatar,0x34)/2;FIELD(header,0x18)=(u32)f.score;
    FIELD(header,0x34)=FIELD(request,0x3c);
    CALL(0x38221,void,(void*,void*))(header,stream);
}
static void clan_data(void *self,void *message) {
    if(!clan_status)fail("GuestOffline: unavailable local clan");
    u32 size=CALL(0x198d19,u32,(void*))(message);
    const u8 *bytes=(const u8*)FIELD(message,8+0x1c);
    struct GuestReader r={bytes,size,0,0,0};
    if(size!=8 || turn_int(&r)!=1 || turn_int(&r)!=clan_serial)
        fail("GuestOffline: unavailable clan identity");
    void *request=decode_packet(14301,0,clan_request,clan_request_size);
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    void *owner=decode_packet(24101,0,saved_home,saved_size),*avatar=(void*)FIELD(owner,0x34);
    if(!avatar || clan_status!=2)fail("GuestOffline: clan data requested before native creation");
    u32 stream[16],member[19]={0};
    CALL(0x192a71,void,(void*,u32))(stream,4096);
    encode_clan_header(stream,request,avatar);
    CALL(0x192d0d,void,(void*,void*))(stream,(void*)FIELD(request,0x34));
    CALL(0x193749,void,(void*,int))(stream,1);
    FIELD(member,0x10)=(u32)CALL(0x99e2d,void*,(void*))(avatar);
    FIELD(member,0x14)=FIELD(member,0x10);
    // getName 0x37f00 reads +0x1c. The preceding nullable string is a
    // different identity field, not the member's displayed player name.
    FIELD(member,0x1c)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    FIELD(member,0x20)=2;FIELD(member,0x24)=(u32)CALL(0x9abcd,int,(void*))(avatar);
    FIELD(member,0x28)=FIELD(avatar,0x34);FIELD(member,0x34)=1;
    FIELD(member,0x48)=(u32)CALL(0x9b4d1,void*,(void*))(avatar);
    CALL(0x37d85,void,(void*,void*))(member,stream);
    reply(self,24301,0,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    CALL(0x19370d,void,(void*))(stream);dispose(owner);dispose(request);
    send_clan_conversation(self);
    // Original 20207 encoder 0xc6a38 contains the current member count only.
    // The actual solo member is present in this running local instance.
    const u8 present=1;reply(self,20207,0,&present,1);
}
static void clan_echo(struct GuestReader *r,int kind) {
    if(!clan_serial || (kind!=208 && !clan_status))fail("GuestOffline: unexpected clan command echo");
    if(turn_int(r)!=1 || turn_int(r)!=clan_serial)fail("GuestOffline: clan echo identity mismatch");
    if(kind==208) {
        if(turn_bool(r))fail("GuestOffline: unsupported kicked-clan echo");
    } else {
        struct ClanFields f=clan_fields(clan_request,clan_request_size);
        if(kind==209) {
            u32 size=turn_int(r);
            if(size!=f.name_size || size>r->size-r->at)fail("GuestOffline: clan echo name extent mismatch");
            for(u32 i=0;i<size;++i)if(turn_byte(r)!=clan_request[f.name_at+i])fail("GuestOffline: clan echo name mismatch");
        }
        if(clan_ref(r,16)!=f.badge || (kind==209 && !turn_bool(r)))fail("GuestOffline: clan echo badge/create mismatch");
    }
    int server=turn_vint(r),tick=turn_vint(r),sequence=turn_vint(r),high=turn_vint(r),low=turn_vint(r);
    // GameMode::addCommand 0x93560..0x93576 assigns the local account to
    // zero-executor commands before immediate execution/EndClientTurn echo.
    if(server< -1 || tick< -1 || sequence< -1 || high!=0 || low!=1)
        fail("GuestOffline: invalid clan command metadata");
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    if(clan_status!=(kind==208?0u:2u))fail("GuestOffline: native clan command did not apply");
}
static void clan_server_metadata(struct SectorWriter *w) {
    const int values[]={-1,-1,-1,0,0};for(int i=0;i<5;++i)sector_vint(w,values[i]);
}
static void leave_clan(void *self,void *message) {
    if(clan_status!=2 || pending_size || CALL(0x198d19,u32,(void*))(message)!=0)
        fail("GuestOffline: invalid solo-clan leave request");
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    struct SectorWriter w={clan_command,0,sizeof clan_command};
    sector_vint(&w,208);clan_id(&w);sector_byte(&w,0);clan_server_metadata(&w);
    clan_command_size=w.size;clan_status=3;write_save();
    reply(self,24111,0,clan_command,clan_command_size);
}
static void edit_clan(void *self,void *message) {
    if(clan_status!=2 || pending_size)fail("GuestOffline: clan edit during another transaction");
    struct ClanFields old=clan_fields(clan_request,clan_request_size);
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    u8 updated[1024];struct SectorWriter w={updated,0,sizeof updated};
    clan_string(&w,clan_request+old.name_at,old.name_size);sector_bytes(&w,data,size);
    struct ClanFields f=clan_fields(updated,w.size);
    capture_current_home(CALL(0x91f7d,void*,(void))(),1);
    clan_request_size=w.size;for(u32 i=0;i<w.size;++i)clan_request[i]=updated[i];
    w=(struct SectorWriter){clan_command,0,sizeof clan_command};
    sector_vint(&w,212);clan_id(&w);sector_vint(&w,16);sector_vint(&w,f.badge);clan_server_metadata(&w);
    clan_command_size=w.size;clan_status=4;write_save();
    reply(self,24111,0,clan_command,clan_command_size);
}
static void search_local_clan(void *self,void *message) {
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader r={data,size,0,0,0};u32 query_size=turn_int(&r);
    if(query_size==0xffffffffu)query_size=0;
    if(query_size>60 || query_size>r.size-r.at)fail("GuestOffline: invalid clan search name extent");
    r.at+=query_size;int region=clan_ref(&r,57);
    int minimum=(int)turn_int(&r),maximum=(int)turn_int(&r),score=(int)turn_int(&r);
    int joinable=turn_byte(&r),reserved1=(int)turn_int(&r),reserved2=(int)turn_int(&r);
    if(minimum<0 || maximum<minimum || maximum>50 || score<0 || score>100000 ||
       joinable>1 || reserved1 || reserved2 || r.at!=r.size)
        fail("GuestOffline: unsupported clan search filters");
    void *owner=0,*request=0,*avatar=0;int match=0;
    if(clan_status==2) {
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);
        owner=decode_packet(24101,0,saved_home,saved_size);avatar=(void*)FIELD(owner,0x34);
        request=decode_packet(14301,0,clan_request,clan_request_size);
        struct ClanFields f=clan_fields(clan_request,clan_request_size);
        match=minimum<=1 && maximum>=1 && score<=(int)FIELD(avatar,0x34)/2 && (region<0 || region==f.region);
        if(joinable && (f.type!=1 || (int)FIELD(avatar,0x34)<f.score))match=0;
        if(query_size) {
            u32 name_lower[5],query_lower[5];
            CALL(0x18ec35,void,(void*,void*))(name_lower,(void*)FIELD(request,0x30));
            CALL(0x18ec35,void,(void*,void*))(query_lower,(void*)FIELD(message,0x30));
            match=match && CALL(0x18ed11,int,(void*,void*))(name_lower,query_lower);
            CALL(0x18db2d,void,(void*))(query_lower);CALL(0x18db2d,void,(void*))(name_lower);
        }
    }
    u32 stream[16];CALL(0x192a71,void,(void*,u32))(stream,4096);
    CALL(0x192d0d,void,(void*,void*))(stream,(void*)FIELD(message,0x30));
    CALL(0x193749,void,(void*,int))(stream,match);
    if(match)encode_clan_header(stream,request,avatar);
    reply(self,24310,0,CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    CALL(0x19370d,void,(void*))(stream);if(request)dispose(request);if(owner)dispose(owner);
}
