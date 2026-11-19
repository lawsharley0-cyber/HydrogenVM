/* Native getCommandType 0x76c4c returns 0x20e = 526. Encode at 0x76c54
 * is base-first, then String and fixed BE target LogicLong. Stage an ID-first
 * KickAllianceMemberMessage; no state changes while decoding the turn. */
static u32 kick_intent(struct GuestReader *r,u8 *output,u32 capacity) {
    u32 at=r->at;clan_skip_string(r,1024,0);
    if(turn_int(r)!=3 || turn_int(r)!=1)fail("GuestOffline: unknown native kick target");
    struct SectorWriter w={output,0,capacity};
    sector_bytes(&w,r->data+r->at-8,8);sector_bytes(&w,r->data+at,r->at-at-8);return w.size;
}
