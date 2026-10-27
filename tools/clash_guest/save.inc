/* Guest-native, versioned, atomic home checkpoint. Included in transport.c so
 * the freestanding payload needs no additional dynamic imports or C++ runtime.
 * This first stage persists native home/avatar data, not host reward journals.
 */
static u8 saved_home[65536];
static u32 saved_size,avatar_offset;
static int save_initialized;
static int committed_wins;
static char save_path[512],temporary_path[520];
static u32 s_getenv,s_open,s_read,s_write,s_flush,s_fileno,s_sync,s_close,s_rename,s_error;
#ifdef GUEST_PROFILE_SAVES
static u32 s_profile_clock;
static u32 checkpoint_calls,checkpoint_total_us,checkpoint_max_us;
static u32 checkpoint_clock_us(void) {
    u32 timeval[2];
    if(CALL(s_profile_clock,int,(void*,void*))(timeval,0)!=0)
        fail("GuestOffline: checkpoint profiling clock failed");
    return timeval[0]*1000000u+timeval[1]; // wrap-safe elapsed intervals below 71 minutes
}
#endif
static u32 symbol(const char *name) {
    u32 value=CALL(API_dlsym,u32,(u32,const char*))(0xfffffffeu,name);
    if(!value)fail("GuestOffline: required standard file API unavailable");
    return value;
}
static void path_append(char *dest,const char *base,const char *suffix,u32 capacity) {
    u32 i=0,j=0;
    while(base[i]) {if(i+1>=capacity)fail("GuestOffline: home path too long");dest[i]=base[i];++i;}
    while(suffix[j]) {if(i+1>=capacity)fail("GuestOffline: save path too long");dest[i++]=suffix[j++];}
    dest[i]=0;
}
static u32 checksum(const u8 *bytes,u32 size) {
    u32 value=2166136261u;
    for(u32 i=0;i<size;++i)value=(value^bytes[i])*16777619u;
    return value;
}
#include "clock.inc"
#include "clan_state.inc"
#include "mail_state.inc"
#include "card_request_state.inc"
#include "friendly_state.inc"
#include "donor_state.inc"
#include "donor_inventory_state.inc"
#include "npc_request_state.inc"
static void write_save(void) {
#ifdef GUEST_PROFILE_SAVES
    const u32 began=checkpoint_clock_us();
#endif
    if(saved_size<avatar_offset+8 || saved_size>sizeof saved_home)
        fail("GuestOffline: invalid native checkpoint extent");
    u32 header[4]={0x31475243u,saved_size,avatar_offset,checksum(saved_home,saved_size)};
    void *file=CALL(s_open,void*,(const char*,const char*))(temporary_path,"wb");
    if(!file)fail("GuestOffline: cannot create checkpoint");
    int valid=CALL(s_write,u32,(const void*,u32,u32,void*))(header,1,sizeof header,file)==sizeof header;
    valid=valid && CALL(s_write,u32,(const void*,u32,u32,void*))(saved_home,1,saved_size,file)==saved_size;
    u32 clock[4]={0x31545247u,saved_epoch,header[3],0}; // GRT1, 32-bit Unix seconds
    if(!saved_epoch)fail("GuestOffline: checkpoint has no accounted clock");
    clock[3]=checksum((const u8*)clock,12);
    valid=valid && CALL(s_write,u32,(const void*,u32,u32,void*))(clock,1,sizeof clock,file)==sizeof clock;
    // A parent marker distinguishes a migrated donor/tombstone from a legacy
    // clan with no donor. Removing the entire trailing GDN1 must not reseed it.
    clan_expects_donor=donor_size!=0;
    clan_expects_donor_inventory=donor_inventory_count!=0;
    clan_expects_npc_request=npc_request_sequence!=0;
    write_clan(file,header[3]);
    write_clan_chat(file,header[3]);
    write_local_mail(file,header[3]);
    write_card_request(file,header[3]);
    write_friendly(file,header[3]);
    write_donor(file,header[3]);
    write_donor_inventory(file,header[3]);
    write_npc_request(file,header[3]);
    if(pending_size) {
        u32 journal[5]={0x314a5247u,pending_size,(u32)pending_kind,(u32)pending_id,checksum(pending_reward,pending_size)};
        valid=valid && CALL(s_write,u32,(const void*,u32,u32,void*))(journal,1,sizeof journal,file)==sizeof journal;
        valid=valid && CALL(s_write,u32,(const void*,u32,u32,void*))(pending_reward,1,pending_size,file)==pending_size;
    }
    valid=valid && CALL(s_flush,int,(void*))(file)==0;
    int fd=CALL(s_fileno,int,(void*))(file);
    valid=valid && fd>=0 && CALL(s_sync,int,(int))(fd)==0;
    int closed=CALL(s_close,int,(void*))(file);
    if(!valid || closed!=0)fail("GuestOffline: checkpoint write/flush failed");
    if(CALL(s_rename,int,(const char*,const char*))(temporary_path,save_path)!=0)
        fail("GuestOffline: atomic checkpoint replacement failed");
#ifdef GUEST_PROFILE_SAVES
    const u32 elapsed=checkpoint_clock_us()-began;
    ++checkpoint_calls;checkpoint_total_us+=elapsed;
    if(elapsed>checkpoint_max_us)checkpoint_max_us=elapsed;
    if(elapsed>=4000 || !(checkpoint_calls&31))
        CALL(API_printf,int,(const char*,u32,u32,u32,u32))(
            "GuestOffline: checkpoint_us=%u call=%u block_total_us=%u max_us=%u\n",
            elapsed,checkpoint_calls,checkpoint_total_us,checkpoint_max_us);
    if(!(checkpoint_calls&31))checkpoint_total_us=0;
#endif
}
static void load_guest_home(void) {
    if(save_initialized)return;
    s_getenv=symbol("getenv");s_open=symbol("fopen");s_read=symbol("fread");
    s_write=symbol("fwrite");s_flush=symbol("fflush");s_fileno=symbol("fileno");
    s_sync=symbol("fsync");s_close=symbol("fclose");s_rename=symbol("rename");s_error=symbol("__error");
    s_wall_clock=symbol("gettimeofday");
#ifdef GUEST_PROFILE_SAVES
    s_profile_clock=symbol("gettimeofday");
#endif
    const char *home=CALL(s_getenv,const char*,(const char*))("HOME");
    if(!home || !home[0])fail("GuestOffline: guest HOME missing");
    path_append(save_path,home,"/Documents/ClashGuestOffline-v1.bin",sizeof save_path);
    path_append(temporary_path,save_path,".tmp",sizeof temporary_path);
    void *file=CALL(s_open,void*,(const char*,const char*))(save_path,"rb");
    if(!file) {
        if(*CALL(s_error,int*,(void))()!=2)fail("GuestOffline: checkpoint cannot be read");
        saved_size=sizeof initial_home;avatar_offset=INITIAL_AVATAR_OFFSET;
        for(u32 i=0;i<saved_size;++i)saved_home[i]=initial_home[i];
        saved_epoch=wall_epoch();
        write_save();
    } else {
        u32 header[4];
        if(CALL(s_read,u32,(void*,u32,u32,void*))(header,1,sizeof header,file)!=sizeof header ||
           header[0]!=0x31475243u || header[1]>sizeof saved_home ||
           header[2]>header[1] || header[1]-header[2]<8)
            fail("GuestOffline: invalid checkpoint header; original file retained");
        saved_size=header[1];avatar_offset=header[2];
        int valid=CALL(s_read,u32,(void*,u32,u32,void*))(saved_home,1,saved_size,file)==saved_size;
        int saw_clock=0,saw_journal=0,saw_clan=0,saw_chat=0,saw_mail=0,saw_request=0,saw_friendly=0,saw_donor=0,saw_inventory=0,saw_npc_request=0;
        for(;;) {
            u32 tag,extra=CALL(s_read,u32,(void*,u32,u32,void*))(&tag,1,4,file);
            if(!extra)break;
            if(extra!=4)fail("GuestOffline: truncated checkpoint extension; original file retained");
            if(tag==0x31545247u && !saw_clock) {
                u32 clock[4]={tag,0,0,0};
                if(CALL(s_read,u32,(void*,u32,u32,void*))(clock+1,1,12,file)!=12 || !clock[1] ||
                   clock[2]!=header[3] || clock[3]!=checksum((const u8*)clock,12))
                    fail("GuestOffline: invalid checkpoint clock; original file retained");
                saved_epoch=clock[1];saw_clock=1;continue;
            }
            if(tag==0x314c4347u && !saw_clan) {
                read_clan(file,header[3]);saw_clan=1;continue;
            }
            if(tag==0x31534347u && !saw_chat) {
                read_clan_chat(file,header[3]);saw_chat=1;continue;
            }
            if(tag==0x31414d47u && !saw_mail) {
                read_local_mail(file,header[3]);saw_mail=1;continue;
            }
            if(tag==0x31524347u && !saw_request) {
                read_card_request(file,header[3]);saw_request=1;continue;
            }
            if(tag==0x31524647u && !saw_friendly) {
                read_friendly(file,header[3]);saw_friendly=1;continue;
            }
            if(tag==0x314e4447u && !saw_donor) {
                read_donor(file,header[3]);saw_donor=1;continue;
            }
            if(tag==0x31494e47u && !saw_inventory) {
                read_donor_inventory(file,header[3]);saw_inventory=1;continue;
            }
            if(tag==0x31524e47u && !saw_npc_request) {
                read_npc_request(file,header[3]);saw_npc_request=1;continue;
            }
            if(tag!=0x314a5247u || saw_journal)
                fail("GuestOffline: unknown/duplicate checkpoint extension; original file retained");
            u32 journal[5]={tag,0,0,0,0};
            valid=valid && CALL(s_read,u32,(void*,u32,u32,void*))(journal+1,1,16,file)==16;
            if(!valid || journal[1]<3 || journal[1]>sizeof pending_reward ||
               journal[2]<1 || journal[2]>3 || (journal[2]>=2 && journal[3]!=0) || journal[3]>1000000)
                fail("GuestOffline: invalid reward journal; original file retained");
            pending_size=journal[1];pending_kind=(int)journal[2];pending_id=(int)journal[3];
            valid=valid && CALL(s_read,u32,(void*,u32,u32,void*))(pending_reward,1,pending_size,file)==pending_size;
            valid=valid && pending_reward[0]==0x95 && pending_reward[1]==3 && checksum(pending_reward,pending_size)==journal[4];
            if(!valid)fail("GuestOffline: incomplete reward journal; original file retained");
            saw_journal=1;
        }
        int closed=CALL(s_close,int,(void*))(file);
        if(clan_expects_donor && !saw_donor)
            fail("GuestOffline: required donor record missing; original retained");
        if(clan_expects_donor_inventory && !saw_inventory)
            fail("GuestOffline: required donor inventory missing; original retained");
        if(clan_expects_npc_request && !saw_npc_request)
            fail("GuestOffline: required NPC request missing; original retained");
        if(saw_chat && (!saw_clan || !clan_status))fail("GuestOffline: conversation has no clan");
        if(!valid || closed!=0 || checksum(saved_home,saved_size)!=header[3])
            fail("GuestOffline: incomplete checkpoint; original file retained");
        if(!saw_clock) {
            // Old files have no durable wall-time evidence. Start accounting
            // now; neither filesystem mtime nor historical time is guessed.
            saved_epoch=wall_epoch();
            CALL(API_puts,int,(const char*))("GuestOffline: legacy clock starts now; no historical time credited");
        }
        CALL(API_puts,int,(const char*))("GuestOffline: loaded guest-owned native checkpoint");
    }
    save_initialized=1;
}
static void serialize_native(void *home,void *avatar,int commit) {
    u32 stream[16];
    CALL(0x192a71,void,(void*,u32))(stream,65536);
    if(home) {
        CALL(0x124cfd,void,(void*,void*))(home,stream);
        avatar_offset=CALL(0x192b2d,u32,(void*))(stream);
    }
    CALL(0x99e51,void,(void*,void*))(avatar,stream);
    u32 size=CALL(0x192b2d,u32,(void*))(stream);
    u8 *data=CALL(0x1936fd,u8*,(void*))(stream);
    u32 prefix=home?0:avatar_offset;
    if(!size || prefix>sizeof saved_home-1 || size>sizeof saved_home-prefix-1)
        fail("GuestOffline: native serializer exceeded checkpoint bound");
    for(u32 i=0;i<size;++i)saved_home[prefix+i]=data[i];
    saved_size=prefix+size+1;saved_home[saved_size-1]=0;
    CALL(0x19370d,void,(void*))(stream);
    if(commit)write_save();
}
static void capture_native(void *home,void *avatar) {serialize_native(home,avatar,1);}
static void capture_current_home(void *ui,int force) {
    static int last_tick=-20;
    if(!save_initialized || battle_started || FIELD(ui,0x28)!=0)return;
    void *logic=(void*)FIELD(ui,0x54);
    if(!logic)return;
    void *home=(void*)FIELD(logic,0x30),*avatar=(void*)FIELD(logic,0x34);
    if(!home || !avatar)return;
    if(home_clock_logic!=logic || home!=delivered_home)return;
    if((int)FIELD(avatar,0x9c)<committed_wins)return;
    if(!clan_capture_ready(avatar))return;
    if(BYTE(home,0xb4)) {
        if(!pending_size && requested_kind) {
            account_home_clock(logic,home,0);
            prepare_reward(home,avatar);
        }
        return;
    }
    if(pending_size) {
        if(!reward_applied)return;
        if(pending_kind==1 && CALL(0x125645,void*,(void*,int))(home,pending_id))return;
        completed_kind=pending_kind;completed_id=pending_id;
        pending_size=0;pending_kind=0;pending_id=0;reward_applied=0;
        force=1;
        CALL(API_puts,int,(const char*))("GuestOffline: native reward applied; committing completed checkpoint");
    }
    int tick=(int)FIELD(logic,0x38);
    if(!force && tick>=last_tick && (u32)tick-(u32)last_tick<20)return;
    last_tick=tick;account_home_clock(logic,home,1);capture_native(home,avatar);
    committed_wins=(int)FIELD(avatar,0x9c);
}
void guest_snapshot(void *ui) {capture_current_home(ui,0);friendly_tick();donation_tick();npc_request_tick();}
void guest_set_npc_losses(void *avatar,int count) {
    if(friendly_state==2 && battle_started && !arena_battle &&
       FIELD(avatar,0x10)==0 && FIELD(avatar,0x14)==1)return;
    int previous=(int)FIELD(avatar,0xa0);FIELD(avatar,0xa0)=count;
    // Original battleEnded calls setNpcLoseCount at 0x14ce2. Only that
    // transition for the local player is an earned loss; constructor/decoders
    // still receive the original setter behavior without checkpoint writes.
    if(((u32)__builtin_return_address(0)&~1u)!=0x14ce6 || !save_initialized || !battle_started ||
       FIELD(avatar,0x10)!=0 || FIELD(avatar,0x14)!=1)return;
    if(count==previous)return;
    if(count!=previous+1)fail("GuestOffline: unexpected native loss transition");
    serialize_native(0,avatar,0);
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30),*copy=(void*)FIELD(owner,0x34);
    if(!home || !copy)fail("GuestOffline: invalid decoded loss checkpoint");
    credit_arena_crowns(copy);
    CALL(0x9b709,void,(void*))(copy);
    capture_native(home,copy);dispose(owner);
    CALL(API_printf,int,(const char*,int))("GuestOffline: persisted original NPC loss=%d\n",count);
}
void guest_set_npc_wins(void *avatar,int count) {
    if(friendly_state==2 && battle_started && !arena_battle &&
       FIELD(avatar,0x10)==0 && FIELD(avatar,0x14)==1)return;
    int previous=(int)FIELD(avatar,0x9c);FIELD(avatar,0x9c)=count;
    if(((u32)__builtin_return_address(0)&~1u)!=0x14c7c || !save_initialized || !battle_started ||
       FIELD(avatar,0x10)!=0 || FIELD(avatar,0x14)!=1)return;
    if(count==previous)return;
    if(count!=previous+1)fail("GuestOffline: unexpected native win transition");
    // Work on decoded copies: advancing the live winner index here would make
    // the victory screen show the following chest rather than this reward.
    serialize_native(0,avatar,0);
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30),*copy=(void*)FIELD(owner,0x34);
    if(!home || !copy || FIELD(home,0x58)!=4)fail("GuestOffline: invalid decoded reward home");
    u32 *slots=(u32*)FIELD(home,0x50);int empty=-1,total=0;
    for(int i=0;i<4;++i){if(slots[i])++total;else if(empty<0)empty=i;}
    if(empty>=0) {
        void *arena=CALL(0x9b4d1,void*,(void*))(copy);
        void *data=CALL(0x9b3b9,void*,(void*,void*))(copy,arena);
        if(!data)fail("GuestOffline: original winner-chest selection failed");
        void *chest=CALL(API_malloc,void*,(u32))(32);
        if(!chest)fail("GuestOffline: chest allocation failed");
        CALL(0xf1671,void,(void*,void*))(chest,data);
        CALL(0xf1ab9,void,(void*,int))(chest,1);
        CALL(0xf1ac1,void,(void*,int))(chest,1);
        CALL(0x124bf1,void,(void*,void*,int))(home,chest,empty);
        CALL(0x9aeb5,void,(void*))(copy);
        CALL(0x9af3d,void,(void*,int))(copy,total+1);
        CALL(API_puts,int,(const char*))("GuestOffline: original winner chest banked");
    }
    // Original battleEnded calls refreshArena at 0x14c7e, immediately after
    // the win setter we intercept. The live avatar will still do that, but
    // this saved copy must also run the original CSV/trophy/NPC-count logic.
    // Otherwise the seventh earned win persists Training Camp forever.
    credit_arena_crowns(copy);
    CALL(0x9b709,void,(void*))(copy);
    capture_native(home,copy);dispose(owner);
    committed_wins=count;
    CALL(API_printf,int,(const char*,int))("GuestOffline: persisted original NPC win=%d\n",count);
}
