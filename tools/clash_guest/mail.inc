/* Native clan mail (client command 533) and its actual local inbox recipient.
 * Original role, body/title validation and 43200-second cooldown execute first.
 * Commit before the outer update can display its result; no external recipient
 * or successful online-send acknowledgement is invented. */
#include "mail_wire.inc"
static u32 encode_local_inbox(u8 *output,u32 capacity) {
    struct SectorWriter w={output,0,capacity};sector_vint(&w,(int)local_mail_count);
    struct GuestReader r={local_mail,local_mail_size,0,0,0};u32 now=wall_epoch();
    for(u32 i=0;i<local_mail_count;++i) {
        u32 stamp=turn_int(&r),size=turn_int(&r);
        if(size<32 || size>4096 || size>r.size-r.at)fail("GuestOffline: invalid local mail record boundary");
        local_mail_entry(&w,r.data+r.at,size,stamp,now);r.at+=size;
    }
    if(r.at!=r.size)fail("GuestOffline: trailing local inbox records");
    return w.size;
}
static void local_inbox(void *self,void *message) {
    if(CALL(0x198d19,u32,(void*))(message)!=0)fail("GuestOffline: invalid local inbox request");
    static u8 output[33024];u32 size=encode_local_inbox(output,sizeof output);
    reply(self,24411,0,output,size);
}
extern int guest_original_mail_execute(void*,void*,int,int);
int guest_mail_execute(void *command,void *logic,int mode,int flags) {
    if((mode&2) && save_initialized && !pending_size && clan_status==2 && home_clock_logic==logic) {
        void *home=CALL(0xd7355,void*,(void*))(logic);
        if(home==delivered_home)account_home_clock(logic,home,1);
    }
    int result=guest_original_mail_execute(command,logic,mode,flags);
    if(result || !(mode&2))return result;
    void *home=CALL(0xd7355,void*,(void*))(logic),*avatar=CALL(0xd742d,void*,(void*))(logic);
    if(!save_initialized || !avatar || FIELD(avatar,0x10)!=0 || FIELD(avatar,0x14)!=1)return result;
    if(!home || home!=delivered_home || home_clock_logic!=logic || clan_status!=2 || pending_size ||
       local_mail_sequence>=1000000000 || CALL(0x126abd,int,(void*))(home)<=0)
        fail("GuestOffline: native mail completed outside a valid local clan transaction");
    u32 stream[16],entry[13]={0},identity[2]={0,++local_mail_sequence};
    CALL(0x192a71,void,(void*,u32))(stream,4096);
    // The echoed client command has these two original strings before its
    // base metadata. Retain their exact encoded digest even after deletion.
    CALL(0x192d0d,void,(void*,void*))(stream,(void*)FIELD(command,0x18));
    CALL(0x192d0d,void,(void*,void*))(stream,(void*)FIELD(command,0x1c));
    local_mail_echo_crc=checksum(CALL(0x1936fd,const u8*,(void*))(stream),CALL(0x192b2d,u32,(void*))(stream));
    CALL(0x19370d,void,(void*))(stream);
    CALL(0x192a71,void,(void*,u32))(stream,4096);CALL(0x193749,void,(void*,int))(stream,6);
    FIELD(entry,4)=(u32)identity;FIELD(entry,8)=(u32)CALL(0x99e2d,void*,(void*))(avatar);
    FIELD(entry,0xc)=(u32)CALL(0x99e45,void*,(void*))(avatar);
    // AvatarStreamEntry's +0x10 integer has no qualified semantic getter;
    // retain its original constructor default instead of inventing a value.
    BYTE(entry,0x19)=1;
    FIELD(entry,0x1c)=FIELD(entry,8);FIELD(entry,0x20)=(u32)CALL(0x9ae01,void*,(void*))(avatar);
    FIELD(entry,0x24)=(u32)CALL(0x9ae31,void*,(void*))(avatar);
    FIELD(entry,0x28)=(u32)CALL(0x9ae39,void*,(void*))(avatar);
    FIELD(entry,0x2c)=FIELD(command,0x18);FIELD(entry,0x30)=FIELD(command,0x1c);
    CALL(0xe3501,void,(void*,void*))(entry,stream);
    u32 size=CALL(0x192b2d,u32,(void*))(stream),now=wall_epoch();
    const u8 *data=CALL(0x1936fd,const u8*,(void*))(stream);
    u8 encoded[4096];struct SectorWriter checked={encoded,0,sizeof encoded};
    local_mail_entry(&checked,data,size,now,now);
    while(local_mail_count>=32 || checked.size+8>sizeof local_mail-local_mail_size) {
        struct GuestReader old={local_mail,local_mail_size,0,0,0};turn_int(&old);u32 drop=turn_int(&old);
        if(!local_mail_count || drop>old.size-old.at)fail("GuestOffline: invalid local mail eviction boundary");
        drop+=8;for(u32 i=drop;i<local_mail_size;++i)local_mail[i-drop]=local_mail[i];
        local_mail_size-=drop;--local_mail_count;
    }
    struct SectorWriter saved={local_mail,local_mail_size,sizeof local_mail};
    clan_int(&saved,now);clan_int(&saved,checked.size);sector_bytes(&saved,encoded,checked.size);
    local_mail_size=saved.size;++local_mail_count;
    // Account any pre-command paused time before the new timer starts. Its
    // fresh cooldown must not inherit that earlier interval retroactively.
    account_home_clock(logic,home,0);capture_native(home,avatar);
    if(!logged_in || !active_messaging || !BYTE(active_messaging,0xe4))fail("GuestOffline: local mail recipient unavailable");
    reply(active_messaging,24412,0,encoded,checked.size);
    CALL(0x19370d,void,(void*))(stream);
    CALL(API_puts,int,(const char*))("GuestOffline: original clan mail and native cooldown committed to local inbox");
    return result;
}
static void mail_echo(struct GuestReader *r) {
    u32 start=r->at;clan_skip_string(r,1024,1);clan_skip_string(r,1024,1);
    if(!local_mail_sequence || checksum(r->data+start,r->at-start)!=local_mail_echo_crc)
        fail("GuestOffline: local mail echo does not match its completed native command");
    int tick=turn_vint(r),sequence=turn_vint(r),high=turn_vint(r),low=turn_vint(r);
    if(tick< -1 || sequence< -1 || high || low!=1)fail("GuestOffline: invalid local mail echo metadata");
}
static void remove_local_mail(void *self,void *message) {
    const u8 *data=(const u8*)FIELD(message,8+0x1c);u32 size=CALL(0x198d19,u32,(void*))(message);
    struct GuestReader query={data,size,0,0,0};
    if(size!=8 || turn_int(&query)!=0)fail("GuestOffline: invalid local inbox removal identity");
    u32 serial=turn_int(&query);
    if(!serial || serial>local_mail_sequence)fail("GuestOffline: unknown local inbox entry");
    struct GuestReader r={local_mail,local_mail_size,0,0,0};
    for(u32 i=0;i<local_mail_count;++i) {
        u32 begin=r.at;turn_int(&r);u32 length=turn_int(&r);
        if(length<9 || length>r.size-r.at)fail("GuestOffline: invalid local inbox removal boundary");
        struct GuestReader entry={r.data+r.at,length,0,0,0};
        if(turn_vint(&entry)!=6 || turn_int(&entry)!=0)fail("GuestOffline: unsupported local inbox entry type");
        u32 identity=turn_int(&entry);r.at+=length;
        if(identity!=serial)continue;
        u32 drop=r.at-begin;
        for(u32 j=r.at;j<local_mail_size;++j)local_mail[j-drop]=local_mail[j];
        local_mail_size-=drop;--local_mail_count;
        capture_current_home(CALL(0x91f7d,void*,(void))(),1);write_save();break;
    }
    // A past, already removed/evicted sequence is genuinely absent. Returning
    // the original removal notification is idempotent, not a fabricated mail.
    reply(self,24418,0,data,size);
}
