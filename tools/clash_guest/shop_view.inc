static void shop_refresh_view(int weekday) {
    void *ui=CALL(0x91f7d,void*,(void))();
    void *screen=CALL(0x93b6d,void*,(void*))(ui);if(!screen)return;
    void *page=CALL(0xe6cf5,void*,(void*))(screen);if(!page)return;
    void *banner=(void*)FIELD(page,0xd4);
    if(weekday!=1 && banner) {
        // The original createItems only replaces this owned MovieClip on
        // Sundays; use its normal deleting destructor when leaving Sunday.
        FIELD(page,0xd4)=0;
        u32 *vtable=(u32*)FIELD(banner,0);
        CALL(vtable[1],void,(void*))(banner);
    }
    // setShopValues notifies inventory listeners before storing the new day.
    // Rebuild via the original routine after that store, for correct header,
    // card positions, scroll bounds and the current countdown.
    CALL(0x8693d,void,(void*))(page);
}
