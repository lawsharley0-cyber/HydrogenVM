static int shop_card_echo(struct GuestReader *r) {
    // BuyCard::encode/decode uses a fixed BE int at +0x1c/0x48 in the
    // encoder/ByteStream vtable, followed by the native card reference.
    if(turn_int(r)!=1)fail("GuestOffline: unsupported native shop purchase selector");
    int table=turn_vint(r),index=turn_vint(r);
    if(table<26 || table>28 || index<0 || index>=1000000)fail("GuestOffline: invalid native shop card");
    return table*1000000+index;
}
static int shop_chest_echo(struct GuestReader *r) {
    if(turn_int(r)!=1)fail("GuestOffline: unsupported native chest purchase selector");
    int table=turn_vint(r),index=turn_vint(r);
    if(table!=19 || index<0 || index>=1000000)fail("GuestOffline: invalid purchased chest reference");
    return 19000000+index;
}
static int shop_pack_echo(struct GuestReader *r) {
    int table=turn_vint(r),index=turn_vint(r);
    if(table!=55 || index<0 || index>=1000000)fail("GuestOffline: invalid resource pack reference");
    return 55000000+index;
}
