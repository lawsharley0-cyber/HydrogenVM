/* Offline help is actual local content in the original game's InfoPopup.
 * The archived online support SDK requires a remote service and unrelated
 * Auto Layout APIs. Do not pretend to connect it or remove its assertions.
 */
extern void guest_original_settings_click(void*,void*);
static void guest_info(const char *heading,const char *text) {
    u32 title[5],body[5];
    CALL(0x18dcf9,void,(void*,const char*))(title,heading);
    CALL(0x18dcf9,void,(void*,const char*))(body,text);
    // Original Facebook error UI allocates exactly 0xa0 for InfoPopup, passes
    // two native String values by address, then destroys the copied arguments.
    void *popup=CALL(API_malloc,void*,(u32))(0xa0);
    if(!popup)fail("GuestOffline: help popup allocation failed");
    CALL(0x7e4b9,void,(void*,void*,void*,int))(popup,title,body,0);
    CALL(0x18db2d,void,(void*))(body);CALL(0x18db2d,void,(void*))(title);
    void *gui=CALL(0x48871,void*,(void))();
    CALL(0x49431,void,(void*,void*,int,int,int))(gui,popup,1,0,0);
}
void guest_settings_click(void *self,void *button) {
    // Pinned SettingsPopup dispatch 0x94fa6 compares its help control at +0xb0.
    // Local settings still execute the original function and all its checks;
    // online-only controls below receive an explicit offline explanation.
    if(button && button==(void*)FIELD(self,0xb0)) {
        guest_info("Offline Help",
            "Battle trainers and open earned chests.\nUpgrade your deck from Cards.\n\n"
            "Progress saves in this instance.\nKeep this instance when updating.\n\n"
            "Online support is unavailable.");
        CALL(API_puts,int,(const char*))("GuestOffline: displayed local help");
        return;
    }
    if(button && button==(void*)FIELD(self,0xac)) {
        guest_info("Offline Account","Facebook sign-in is unavailable in this offline edition.\n\nYour progress stays in this game's instance.");
        return;
    }
    if(button && button==(void*)FIELD(self,0xa8)) {
        guest_info("Move Your Progress","Cloud linking is unavailable in this offline edition.\n\nBack up this game's instance with HydrogenVM's file tools before moving or removing it.");
        return;
    }
    if(button && (button==(void*)FIELD(self,0xc0) || button==(void*)FIELD(self,0xc4) || button==(void*)FIELD(self,0xc8))) {
        guest_info("Online Page","The original web page needs an internet connection and cannot open in this offline edition.");
        return;
    }
    guest_original_settings_click(self,button);
}
