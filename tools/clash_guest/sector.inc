/* Reassemble the source-derived sector around the actual selected native deck.
 * No card stats are synthesized: retain original card bytes and use CSV-derived
 * tower level/HP tables validated by the private build-time exporter.
 */
struct GuestDeck {u32 offsets[8],lengths[8];int count;};
static void skip_card(struct GuestReader *r) {
    turn_reference(r);for(int i=0;i<5;++i)turn_vint(r);turn_bool(r);turn_bool(r);
}
static struct GuestDeck selected_deck(void) {
    struct GuestReader r={saved_home,avatar_offset,0,0,0};
    struct GuestDeck decks[3]={0};
    for(int i=0;i<8;++i)turn_byte(&r);
    for(int i=0;i<6;++i)turn_vint(&r);
    int count=turn_vint(&r);
    if(count<1 || count>3)fail("GuestOffline: invalid saved deck count");
    for(int i=0;i<count;++i) {
        int present[8];for(int j=0;j<8;++j)present[j]=turn_bool(&r);
        for(int j=0;j<8;++j)if(present[j]) {
            int at=decks[i].count++;decks[i].offsets[at]=r.at;
            skip_card(&r);decks[i].lengths[at]=r.at-decks[i].offsets[at];
        }
    }
    int collection=turn_vint(&r);
    if(collection<0 || collection>=200)fail("GuestOffline: invalid saved card collection");
    for(int i=0;i<collection;++i)skip_card(&r);
    turn_vint(&r);int selected=turn_vint(&r);
    if(selected<0 || selected>=count || decks[selected].count<1)
        fail("GuestOffline: invalid selected deck");
    return decks[selected];
}
static int player_level(void) {
    struct GuestReader r={saved_home+avatar_offset,saved_size-avatar_offset-1,0,0,0};
    for(int i=0;i<6;++i)turn_vint(&r);
    u32 length=0;for(int i=0;i<4;++i)length=(length<<8)|turn_byte(&r);
    if(length>r.size-r.at)fail("GuestOffline: invalid avatar name extent");
    r.at+=length;turn_bool(&r);turn_vint(&r);turn_reference(&r);turn_vint(&r);turn_bool(&r);
    int arrays=turn_vint(&r);
    if(arrays<0 || arrays>16)fail("GuestOffline: invalid avatar resource arrays");
    for(int i=0;i<arrays;++i) {
        int count=turn_vint(&r);if(count<0 || count>1000)fail("GuestOffline: invalid avatar resources");
        for(int j=0;j<count;++j){turn_reference(&r);turn_vint(&r);}
    }
    turn_vint(&r);turn_vint(&r);turn_vint(&r);int level=turn_vint(&r);
    if(level<1 || level>GUEST_MAX_TOWER_LEVEL)fail("GuestOffline: unsupported player tower level");
    return level;
}
struct SectorWriter {u8 *data;u32 size,capacity;};
static void sector_byte(struct SectorWriter *w,u8 value) {
    if(w->size==w->capacity)fail("GuestOffline: sector exceeds bound");
    w->data[w->size++]=value;
}
static void sector_bytes(struct SectorWriter *w,const u8 *data,u32 size) {
    if(size>w->capacity-w->size)fail("GuestOffline: sector exceeds bound");
    for(u32 i=0;i<size;++i)sector_byte(w,data[i]);
}
static void sector_vint(struct SectorWriter *w,int value) {
    u32 magnitude=value<0?0u-(u32)value:(u32)value;
    int count=1;const int widths[]={6,13,20,27};
    for(int i=0;i<4;++i){if(magnitude<(1u<<widths[i]))break;++count;}
    u32 encoded=(u32)value&0x7fffffffu;
    sector_byte(w,(encoded&63)|(value<0?64:0)|(count>1?128:0));
    for(int i=1;i<count;++i)
        sector_byte(w,((encoded>>(6+(i-1)*7))&(i==4?15:127))|(i<count-1?128:0));
}
static void sector_part(struct SectorWriter *w,const struct GuestSector *s,int part,int level) {
    u32 base=w->size;sector_bytes(w,s->parts[part],s->sizes[part]);
    for(u32 i=0;i<s->patchCount;++i) {
        const struct GuestTowerPatch *p=&s->patches[i];
        if(p->part!=(u32)part)continue;
        if(p->offset>s->sizes[part] || p->width>s->sizes[part]-p->offset)
            fail("GuestOffline: tower patch exceeds template");
        struct SectorWriter patch={w->data+base+p->offset,0,p->width};
        sector_vint(&patch,p->values[level-1]);
        if(patch.size!=p->width)fail("GuestOffline: tower patch width mismatch");
    }
}
static u32 assemble_sector_template(u8 *bytes,u32 capacity,const struct GuestSector *s) {
    struct SectorWriter w={bytes,0,capacity};
    struct GuestDeck deck=selected_deck();int level=player_level(),hand=deck.count<4?deck.count:4;
    sector_part(&w,s,0,level);sector_byte(&w,1);sector_vint(&w,hand);
    for(int i=0;i<4;++i)sector_vint(&w,i<hand?i:-1);
    sector_vint(&w,deck.count-hand);for(int i=hand;i<deck.count;++i)sector_vint(&w,i);
    const int trailer[]={0,-1,0,0,0,5,0,0};
    for(int i=0;i<8;++i)sector_vint(&w,trailer[i]);
    sector_part(&w,s,1,level);
    u32 mask=(1u<<deck.count)-1;
    sector_byte(&w,(u8)(s->lowBits|(mask<<3)));sector_byte(&w,(u8)(mask>>5));
    for(int i=0;i<deck.count;++i) {
        u32 length=deck.lengths[i];
        if(!length)fail("GuestOffline: empty selected card encoding");
        sector_bytes(&w,saved_home+deck.offsets[i],length-1);
        // A following deck's slot flags may share this final boolean byte in
        // the home. Only the two card flags belong in the battle deck.
        sector_byte(&w,saved_home[deck.offsets[i]+length-1]&3);
    }
    sector_part(&w,s,2,level);
    sector_bytes(&w,saved_home+avatar_offset,saved_size-avatar_offset-1);
    sector_part(&w,s,3,level);
    CALL(API_printf,int,(const char*,int,int))("GuestOffline: native deck cards=%d tower level=%d\n",deck.count,level);
    return w.size;
}
static u32 assemble_sector(u8 *bytes,u32 capacity,int mission) {
    if(mission<0 || mission>=7)fail("GuestOffline: unsupported training sector");
    return assemble_sector_template(bytes,capacity,&npc_sectors[mission]);
}
