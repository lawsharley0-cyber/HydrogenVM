/* Native ChallengeStreamEntry/ChallengeDoneStreamEntry boundary validation.
 * Stored ages are zero; delivery recomputes them from the durable timestamp. */
static u32 friendly_entry_wire(struct SectorWriter *w,const u8 *data,u32 size,u32 stamp,u32 now) {
    struct GuestReader r={data,size,0,0,0};int type=turn_vint(&r);
    if(type!=10 && type!=11)fail("GuestOffline: invalid friendly stream type");
    if((u32)turn_vint(&r)!=friendly_namespace || (u32)turn_vint(&r)!=friendly_sequence)
        fail("GuestOffline: invalid friendly stream identity");
    for(int i=0;i<2;++i)if(turn_vint(&r)!=0 || turn_vint(&r)!=1)
        fail("GuestOffline: invalid friendly stream author/home");
    clan_skip_string(&r,60,1);int level=turn_vint(&r),role=turn_vint(&r);
    if(level<1 || level>12 || role<1 || role>4)fail("GuestOffline: invalid friendly stream attributes");
    u32 age_at=r.at;if(turn_vint(&r)!=0)fail("GuestOffline: invalid friendly stored age");
    u32 age_end=r.at;if(turn_bool(&r))fail("GuestOffline: removed friendly stream entry");
    if(type==10) {
        clan_skip_string(&r,512,0);int closed=turn_bool(&r);
        if(closed!=(friendly_state>=4))fail("GuestOffline: friendly closed state mismatch");
        if(turn_bool(&r))clan_skip_string(&r,512,0);
    } else {
        if(friendly_state!=3)fail("GuestOffline: friendly result outside completed state");
        clan_skip_string(&r,sizeof friendly_result,1);
        for(int i=0;i<4;++i)turn_int(&r);
        if(turn_bool(&r)) {
            turn_int(&r);for(int i=0;i<8;++i)turn_byte(&r);
        }
    }
    if(r.at!=r.size || !stamp)fail("GuestOffline: invalid friendly stream extent/timestamp");
    sector_bytes(w,data,age_at);u32 elapsed=now>stamp?now-stamp:0;
    sector_vint(w,(int)(elapsed>2147483647u?2147483647u:elapsed));
    sector_bytes(w,data+age_end,size-age_end);return age_at;
}
static u32 friendly_sector_type(u8 *data,u32 size) {
    struct GuestReader r={data,size,0,0,0};
    for(int i=0;i<4;++i)turn_vint(&r);turn_int(&r);turn_vint(&r);
    for(int i=0;i<3;++i)turn_reference(&r);
    if(turn_vint(&r)!=-1 || turn_vint(&r)!=0 || turn_vint(&r)!=0 || turn_vint(&r)!=1)
        fail("GuestOffline: native friendly account prefix mismatch");
    u32 offset=r.at;
    if(turn_vint(&r)!=0)fail("GuestOffline: native friendly battle-type mismatch");
    return offset;
}
static void friendly_echo(struct GuestReader *r) {
    if(friendly_state!=1)fail("GuestOffline: unexpected friendly command echo");
    u32 start=r->at;if(turn_bool(r))clan_skip_string(r,512,0);
    if(checksum(r->data+start,r->at-start)!=friendly_echo_crc)fail("GuestOffline: friendly echo payload mismatch");
    // LogicChallengeCommand::encode tail-calls LogicCommand, not LogicServerCommand.
    int tick=turn_vint(r),sequence=turn_vint(r),high=turn_vint(r),low=turn_vint(r);
    if(tick< -1 || sequence< -1 || high!=0 || low!=1)
        fail("GuestOffline: invalid friendly command metadata");
}
