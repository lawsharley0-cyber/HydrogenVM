/* Validate before any migration/write. Legacy donors could only spend their
 * six initial materials: stock must still be 0/1 and native total donations
 * must equal missing materials. Never adopt arbitrary stock as a new baseline. */
static void validate_donor_inventory(void) {
    if(!donor_size) {
        if(donor_inventory_count)fail("GuestOffline: donor inventory has no native actor");
        return;
    }
    if(donor_inventory_loaded &&
       donor_inventory_body_hash!=checksum(donor_home,donor_size))
        fail("GuestOffline: donor inventory native-body binding mismatch");
    void *packet=decode_packet(24101,0,donor_home,donor_size);
    void *home=(void*)FIELD(packet,0x30),*avatar=(void*)FIELD(packet,0x34);
    void *deck=CALL(0x1255fd,void*,(void*))(home),*collection=CALL(0x125609,void*,(void*))(home);
    int deck_count=CALL(0x16c55,int,(void*))(deck),collection_count=(int)FIELD(collection,8);
    if(deck_count<0 || deck_count>6 || collection_count<0 || collection_count>6 || deck_count+collection_count!=6)
        fail("GuestOffline: donor inventory does not contain exactly the six starter cards");
    for(u32 i=0;i<6;++i) {
        void *card=donation_find_card(home,(int)donor_starter_cards[i]);
        if(!card)fail("GuestOffline: donor starter card missing");
        u32 stock=FIELD(card,0xc),expected;
        if(!donor_inventory_count) {
            if(stock>1)fail("GuestOffline: legacy donor stock cannot establish provenance");
            donor_inventory[i]=(struct DonorInventoryRow){donor_starter_cards[i],1u-stock,0};
        }
        if(!donor_inventory_stock(donor_inventory+i,&expected) || stock!=expected)
            fail("GuestOffline: donor stock differs from committed inventory provenance");
    }
    u32 total_sent;
    if(!donor_inventory_valid(donor_inventory,&total_sent) ||
       (u32)CALL(0x9b861,int,(void*))(avatar)!=total_sent)
        fail("GuestOffline: native donor total differs from committed sent cards");
    dispose(packet);donor_inventory_count=6;
    // The binding applies to the loaded snapshot only. Subsequent native role,
    // clock and transfer changes receive fresh bindings in the same checkpoint.
    donor_inventory_body_hash=0;donor_inventory_loaded=0;
}
static struct DonorInventoryRow *donor_inventory_row(u32 card) {
    if(donor_inventory_count!=6)fail("GuestOffline: donor inventory provenance missing");
    for(u32 i=0;i<6;++i)if(donor_inventory[i].card==card)return donor_inventory+i;
    fail("GuestOffline: donation card outside original donor inventory");return 0;
}
static void donor_inventory_sent(u32 card,u32 before) {
    struct DonorInventoryRow *row=donor_inventory_row(card);u32 expected,total;
    if(!donor_inventory_valid(donor_inventory,&total) || !donor_inventory_stock(row,&expected) ||
       expected!=before || !before || total==1000000000u)
        fail("GuestOffline: donation does not conserve proven inventory");
    ++row->sent;
}
