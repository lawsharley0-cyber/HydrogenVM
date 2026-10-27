/* First earned arena port. Original CSV sector/AI/towers and actual native
 * commodity accounting; no invented match acknowledgment or free entry.
 */
static void start_arena_match(void *self) {
    if(battle_started || pending_size || clan_status==1 || clan_status==3 || clan_status==4)
        fail("GuestOffline: arena entry during an incomplete transaction");
    void *owner=decode_packet(24101,0,saved_home,saved_size);
    void *home=(void*)FIELD(owner,0x30),*avatar=(void*)FIELD(owner,0x34);
    if(!home || !avatar || FIELD(avatar,0x9c)<7)fail("GuestOffline: arena entry before earned graduation");
    void *arena=CALL(0x9b4d1,void*,(void*))(avatar);
    if(!arena || CALL(0x7d2e1,int,(void*))(arena)!=54000001)
        fail("GuestOffline: later arenas require their guest port");
    int score=(int)FIELD(avatar,0x34);
    int cost=CALL(0x123e9d,int,(void*))(arena);
    int gold=CALL(0x9ab55,int,(void*))(avatar),free=CALL(0x9ab7d,int,(void*))(avatar);
    if(score<0 || score>100000 || cost<=0 || cost>500000 || gold<cost || free<0 || free>gold)
        fail("GuestOffline: invalid or insufficient arena entry balance");
    struct OpponentHistory next=opponent_history;
    if(!next.rng)next.rng=wall_epoch()^checksum(saved_home,saved_size)^0xa341316cu;
    if(!opponent_choose(&next,arena1_npc_trophies,arena1_deck_ids,GUEST_ARENA_PROFILE_COUNT,score))
        fail("GuestOffline: no legal varied arena opponent");
    void *gold_data=CALL(0xb8309,void*,(void))();
    if(!gold_data)fail("GuestOffline: missing original Gold resource");
    // Original helper: resource array 0, Gold data, negative change. Its
    // FreeGold clamp is retained too; the final parameter is no bonus context.
    CALL(0x9a5ed,int,(void*,int,void*,int,int))(avatar,0,gold_data,-cost,0);
    int actual=CALL(0x9ab55,int,(void*))(avatar),actual_free=CALL(0x9ab7d,int,(void*))(avatar);
    if(actual!=gold-cost || actual_free!=(free<actual?free:actual))
        fail("GuestOffline: native arena entry accounting mismatch");
    serialize_native(home,avatar,0);
    opponent_history=next;opponent_enabled=1;
    u32 size=prepare_sector_packet_named(&arena1_sectors[next.variant],opponent_names[next.name],next.seed);
    // A validated complete sector is ready before the debit is committed.
    // Interrupted/abandoned matches retain the original paid entry.
    write_save();
    reply(self,21903,1,sector_packet,size);battle_started=1;arena_battle=1;
    dispose(owner);
    CALL(API_printf,int,(const char*,u32,int,u32,u32,const char*,u32,int,int))(
        "GuestOffline: Arena 1 match=%u bot=%d deck=%u style=%u name=%s seed=%u native entry=%d gold=%d\n",
        next.serial,arena1_missions[next.variant],arena1_deck_ids[next.variant],next.style,opponent_names[next.name],next.seed,cost,actual);
}

extern void guest_original_battle_ended(void*,void*);
static void friendly_native_log(void *battle);
void guest_battle_ended(void *battle,void *avatar) {
    // LogicGameMode::battleEnded (0xd756c) explicitly passes null here; the
    // original routine resolves the owner itself. Qualify battle.account,
    // not this optional argument, and forward it unchanged.
    if(friendly_state==2 && battle_started) {
        if(ending_battle)fail("GuestOffline: reentrant friendly battle result");
        friendly_native_log(battle);
        ending_battle=battle;guest_original_battle_ended(battle,avatar);
        friendly_battle_completed(battle);ending_battle=0;return;
    }
    if(!arena_battle || !battle_started) {
        guest_original_battle_ended(battle,avatar);return;
    }
    void *account=(void*)FIELD(battle,0x10);
    if(!account || FIELD(account,0)!=0 || FIELD(account,4)!=1 || ending_battle)
        fail("GuestOffline: arena battle result owner mismatch");
    ending_battle=battle;
    guest_original_battle_ended(battle,avatar);
    ending_battle=0;
}
static void credit_arena_crowns(void *avatar) {
    if(!arena_battle)return; // original training does not award crown progress
    if(!ending_battle)fail("GuestOffline: arena result has no original battle");
    void *summoner=(void*)FIELD(ending_battle,0x38),*objects=(void*)FIELD(ending_battle,0x34);
    if(!summoner || !objects)fail("GuestOffline: incomplete native crown result");
    int crowns=CALL(0x10f3fd,int,(void*,void*))(summoner,objects);
    int pending=CALL(0x9b081,int,(void*))(avatar);
    if(crowns<0 || crowns>3 || pending<0 || pending>3 || crowns>3-pending)
        fail("GuestOffline: invalid native arena crown transfer");
    if(crowns) {
        CALL(0x9b0a9,void,(void*,int))(avatar,pending+crowns);
        if(CALL(0x9b081,int,(void*))(avatar)!=pending+crowns)
            fail("GuestOffline: native pending crown setter mismatch");
    }
    CALL(API_printf,int,(const char*,int,int))(
        "GuestOffline: actual native arena crowns=%d pending=%d\n",crowns,pending+crowns);
}
