/* Add durable definitions before OwnHomeData reaches the client UI, so the
 * collection is built with the new cards on the first restart. This decoded
 * home has no live GameMode; update the supplied native avatar explicitly. */
static void forge_grant_pending(void *home,void *avatar) {
    if(!forge_loaded_count || FIELD(avatar,0x9c)<7 || pending_size ||
       friendly_state==1 || friendly_state==2 || BYTE(home,0xb4))return;
    for(u32 slot=0;slot<forge_loaded_count;++slot) {
        char tid[24];forge_identifier(tid,"TID_MODLAB_",slot);
        void *name=CALL(0x50ce5,void*,(const char*))(tid);
        if(!name || !CALL(0x18e0a9,int,(void*,const char*))(name,forge_records[slot].name))
            fail("Card Forge: native display name mismatch; game save retained");
        void *data=CALL(0xb821d,void*,(int))((int)forge_ids[slot]);
        if(!data)fail("Card Forge: native definition missing; game save retained");
        if(CALL(0x1262c1,void*,(void*,void*))(home,data))continue;
        mod_backup();
        void *card=CALL(0x133749,void*,(void*,void*))(0,data);
        if(!card || FIELD(card,4)!=0)fail("Card Forge: native reward construction failed");
        void *collection=CALL(0x125609,void*,(void*))(home);
        if(!collection)fail("Card Forge: native collection missing");
        CALL(0x83439,void,(void*,void*))(collection,card);
        int total=CALL(0x125f71,int,(void*))(home);
        if(total>CALL(0x9b81d,int,(void*))(avatar))CALL(0x9b845,void,(void*,int))(avatar,total);
        capture_native(home,avatar);
        CALL(API_printf,int,(const char*,u32))("Card Forge: checkpointed native ownership id=%u\n",forge_ids[slot]);
    }
}
