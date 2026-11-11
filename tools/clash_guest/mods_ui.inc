/* Guest UIKit UI; no host settings service or external application required. */
typedef struct {float x,y,w,h;} ModRect;
typedef struct {float w,h;} ModSize;
static u32 mod_msg,mod_sel_api,mod_class_api,mod_window,mod_old_window,mod_target;
static u32 mod_unit_label,mod_status_label,mod_toggle,mod_gold_field,mod_gem_field,forge_name_field,forge_create_button;
static u32 forge_art_button;
static u32 mod_fields[MOD_FIELDS],mod_minus[MOD_FIELDS],mod_plus[MOD_FIELDS];
static struct ModRecord mod_draft[MOD_UNIT_CAPACITY];static u32 mod_draft_enabled;
static u32 mod_sel(const char *s){return CALL(mod_sel_api,u32,(const char*))(s);}
static u32 mod_class(const char *s){return CALL(mod_class_api,u32,(const char*))(s);}
static u32 mod_send0(u32 p,const char *s){return CALL(mod_msg,u32,(u32,u32))(p,mod_sel(s));}
static u32 mod_send1(u32 p,const char *s,u32 a){return CALL(mod_msg,u32,(u32,u32,u32))(p,mod_sel(s),a);}
static u32 mod_string(const char *s){return mod_send1(mod_class("NSString"),"stringWithUTF8String:",(u32)s);}
static void mod_text(u32 p,const char *s){mod_send1(p,"setText:",mod_string(s));}
static void mod_title(u32 p,const char *s){CALL(mod_msg,void,(u32,u32,u32,u32))(p,mod_sel("setTitle:forState:"),mod_string(s),0);}
static void mod_frame(u32 p,ModRect r){CALL(mod_msg,void,(u32,u32,ModRect))(p,mod_sel("setFrame:"),r);}
static u32 mod_color(float r,float g,float b){return CALL(mod_msg,u32,(u32,u32,float,float,float,float))(mod_class("UIColor"),mod_sel("colorWithRed:green:blue:alpha:"),r,g,b,1.0f);}
static void mod_number(char *out,u32 n) {
    char temp[12];u32 count=0;do{u32 d=mod_div(n,10);temp[count++]=(char)('0'+n-d*10);n=d;}while(n);
    for(u32 i=0;i<count;++i)out[i]=temp[count-1-i];out[count]=0;
}
static void mod_set_number(u32 field,u32 n){char text[12];mod_number(text,n);mod_text(field,text);}
static int mod_read_number(u32 field,u32 *n){u32 text=mod_send0(field,"text");return mod_parse_number((const char*)mod_send0(text,"UTF8String"),n);}
static u32 mod_label(u32 parent,const char *text,ModRect frame,float size) {
    u32 label=mod_send0(mod_send0(mod_class("UILabel"),"alloc"),"init");mod_frame(label,frame);
    mod_text(label,text);mod_send1(label,"setTextColor:",mod_color(0.94f,0.96f,1.0f));
    mod_send1(label,"setBackgroundColor:",mod_send0(mod_class("UIColor"),"clearColor"));mod_send1(label,"setOpaque:",0);
    mod_send1(label,"setFont:",CALL(mod_msg,u32,(u32,u32,float))(mod_class("UIFont"),mod_sel("systemFontOfSize:"),size));
    mod_send1(parent,"addSubview:",label);return label;
}
static u32 mod_button(u32 parent,const char *text,int tag,ModRect frame) {
    u32 button=mod_send1(mod_class("UIButton"),"buttonWithType:",0);mod_frame(button,frame);mod_title(button,text);
    CALL(mod_msg,void,(u32,u32,u32,u32))(button,mod_sel("setTitleColor:forState:"),mod_color(0.94f,0.96f,1.0f),0);
    mod_send1(button,"setTag:",(u32)tag);mod_send1(button,"setBackgroundColor:",mod_color(0.14f,0.30f,0.50f));
    CALL(mod_msg,void,(u32,u32,u32,u32,u32))(button,mod_sel("addTarget:action:forControlEvents:"),mod_target,mod_sel("modPress:"),1u<<6);
    mod_send1(parent,"addSubview:",button);return button;
}
static u32 mod_field(u32 parent,ModRect frame,u32 initial) {
    u32 f=mod_send0(mod_send0(mod_class("UITextField"),"alloc"),"init");mod_frame(f,frame);
    mod_send1(f,"setBorderStyle:",3);mod_send1(f,"setBackgroundColor:",mod_color(0.96f,0.97f,1.0f));
    mod_send1(f,"setTextColor:",mod_color(0.05f,0.08f,0.12f));mod_send1(f,"setKeyboardType:",4);
    mod_send1(f,"setTextAlignment:",1);mod_set_number(f,initial);mod_send1(parent,"addSubview:",f);return f;
}
static int mod_read_draft(void) {
    for(u32 f=0;f<MOD_FIELDS;++f){u32 n;if(!mod_read_number(mod_fields[f],&n)||!mod_percent_valid(f,n)){
        mod_message("Use 10-800%; troop size uses 25-400%.");return 0;}mod_draft[mod_selected].value[f]=n;}
    return 1;
}
static void mod_ui_refresh(int stats) {
    mod_text(mod_status_label,mod_status);
    if(!stats)return;
    mod_text(mod_unit_label,mod_name(mod_selected));
    mod_send1(forge_create_button,"setEnabled:",mod_selected<GUEST_MOD_UNIT_COUNT);
#ifdef GUEST_ART_SOURCE_TOKEN
    mod_send1(forge_art_button,"setEnabled:",mod_selected>=GUEST_MOD_UNIT_COUNT);
#endif
    mod_title(mod_toggle,mod_draft_enabled?"Mods: ON":"Mods: OFF");
    for(u32 f=0;f<MOD_FIELDS;++f) {
        mod_set_number(mod_fields[f],mod_draft[mod_selected].value[f]);
        int enabled=(f!=MOD_MOVE || mod_original[mod_selected][0]) && (f!=MOD_SHOT_SPEED || mod_shots[mod_selected]);
        mod_send1(mod_fields[f],"setEnabled:",enabled);mod_send1(mod_minus[f],"setEnabled:",enabled);mod_send1(mod_plus[f],"setEnabled:",enabled);
    }
}
#ifdef GUEST_ART_SOURCE_TOKEN
#include "photo_import.inc"
#endif
#ifdef GUEST_MECH_CARD_COUNT
#include "mechanics_ui.inc"
#ifdef GUEST_SPELL_COUNT
#include "spells_ui.inc"
#endif
#endif
static void mod_close(void){mod_send1(mod_window,"endEditing:",1);mod_send1(mod_window,"setHidden:",1);if(mod_old_window)mod_send0(mod_old_window,"makeKeyAndVisible");}
static void mod_action(u32 self,u32 selector,u32 sender) {
    (void)self;(void)selector;int tag=(int)mod_send0(sender,"tag");
    if(tag==10){mod_close();return;}
#ifdef GUEST_SPELL_COUNT
    if(tag==41){spell_ui_open();return;}
    if(tag>=300 && tag<=345){spell_ui_action(tag);return;}
    if(tag==40 && spell_panel)mod_send1(spell_panel,"setHidden:",1);
#endif
#ifdef GUEST_MECH_CARD_COUNT
    if(tag==40 || tag>=400){mech_ui_action(tag);return;}
#endif
#ifdef GUEST_ART_SOURCE_TOKEN
    if(tag==31){photo_open();return;}
    if(tag==32 || tag==33){photo_choose((u32)(tag-32));return;}
    if(tag==34){mod_send1(photo_panel,"setHidden:",1);return;}
#endif
    if(tag==30) {
        void *home,*avatar;
        const char *name=(const char*)mod_send0(mod_send0(forge_name_field,"text"),"UTF8String");
        u32 bytes=0;if(name)while(bytes<=96 && name[bytes])++bytes;
        if(!name || !forge_name_valid(name,bytes))mod_message("Name: 1-32 characters, max 96 UTF-8 bytes.");
        else if(mod_selected>=GUEST_MOD_UNIT_COUNT)mod_message("Choose an original blueprint to clone.");
        else if(forge_count>=FORGE_MAX_CARDS)mod_message("Maximum 16 custom cards per profile.");
        else if(!mod_home(&home,&avatar))mod_message("Finish pending rewards and return home first.");
        else {
            capture_native(home,avatar);mod_backup();
            if(forge_append(mod_unit_defs[mod_selected].cardId,name)) {
                mod_send1(mod_window,"endEditing:",1);mod_text(forge_name_field,"");
                mod_message("Saved. Restart Clash to load the card.");
            } else mod_message("Cannot save definition. Previous registry retained.");
        }
        mod_ui_refresh(0);return;
    }
    if(tag>=20 && tag<=24) {
        u32 gold,gems;if(!mod_read_number(mod_gold_field,&gold)||!mod_read_number(mod_gem_field,&gems))mod_message("Enter whole positive numbers, or zero.");
        else if(tag==24)mod_currency(gold,gems);
        else {
            u32 *n=tag<22?&gold:&gems,step=tag<22?100:10;
            if(tag==20||tag==22)*n=*n>step?*n-step:0;else if(*n<=1000000-step)*n+=step;
            mod_set_number(mod_gold_field,gold);mod_set_number(mod_gem_field,gems);
        }
        mod_ui_refresh(0);return;
    }
    if(tag!=15 && !mod_read_draft()){mod_ui_refresh(0);return;}
    if(tag==11)mod_draft_enabled=!mod_draft_enabled;
    else if(tag==12)mod_selected=mod_selected?mod_selected-1:mod_count()-1;
    else if(tag==13)mod_selected=mod_selected+1<mod_count()?mod_selected+1:0;
    else if(tag==14) {
        void *home,*avatar;if(!mod_home(&home,&avatar)){mod_message("Finish pending rewards and return home first.");mod_ui_refresh(0);return;}
        if(!mod_archive_rejected()){mod_ui_refresh(0);return;}
        capture_native(home,avatar);mod_backup();
        for(u32 i=0;i<mod_count();++i)mod_records[i]=mod_draft[i];mod_enabled=mod_draft_enabled;
        mod_save();mod_apply_units();mod_message("Unit settings saved for the next battle.");
    } else if(tag==15) {
        for(u32 i=0;i<mod_count();++i)for(u32 f=0;f<MOD_FIELDS;++f)mod_draft[i].value[f]=100;
        mod_draft_enabled=0;mod_reset_requested=1;mod_message("Defaults staged. Press Apply units to save.");
    } else if(tag>=100 && tag<100+2*MOD_FIELDS) {
        u32 field=(u32)(tag-100)/2,n=mod_draft[mod_selected].value[field];
        if(tag&1)n=n<=775?n+25:800;else n=n>=35?n-25:10;
        if(field==MOD_SIZE){if(n<25)n=25;if(n>400)n=400;}
        mod_draft[mod_selected].value[field]=n;
    }
    mod_ui_refresh(1);
}
static void mod_ui_open(void) {
    if(!mod_allowed()){mod_message("Mod Lab unlocks after the seven trainers.");return;}
    mod_initialize();
#ifdef GUEST_SPELL_COUNT
    if(spell_panel)mod_send1(spell_panel,"setHidden:",1);
#endif
#ifdef GUEST_ART_SOURCE_TOKEN
    if(photo_panel)mod_send1(photo_panel,"setHidden:",1);
#endif
    mod_reset_requested=0;
    mod_msg=symbol("objc_msgSend");mod_sel_api=symbol("sel_registerName");mod_class_api=symbol("objc_getClass");
    mod_old_window=mod_send0(mod_send0(mod_class("UIApplication"),"sharedApplication"),"keyWindow");
    for(u32 i=0;i<mod_count();++i)mod_draft[i]=mod_records[i];mod_draft_enabled=mod_enabled;
    if(!mod_window) {
        u32 cls=CALL(symbol("objc_allocateClassPair"),u32,(u32,const char*,u32))(mod_class("NSObject"),"ClashGuestModActions",0);
        if(!cls || !CALL(symbol("class_addMethod"),int,(u32,u32,u32,const char*))(cls,mod_sel("modPress:"),(u32)mod_action,"v@:@"))fail("Mod Lab: action registration failed");
#ifdef GUEST_ART_SOURCE_TOKEN
        if(!CALL(symbol("class_addMethod"),int,(u32,u32,u32,const char*))(cls,mod_sel("imagePickerController:didFinishPickingMediaWithInfo:"),(u32)photo_finished,"v@:@@") || !CALL(symbol("class_addMethod"),int,(u32,u32,u32,const char*))(cls,mod_sel("imagePickerControllerDidCancel:"),(u32)photo_cancelled,"v@:@"))fail("Photo delegate registration failed");
#endif
        CALL(symbol("objc_registerClassPair"),void,(u32))(cls);mod_target=mod_send0(mod_send0(cls,"alloc"),"init");
        mod_window=mod_send0(mod_send0(mod_class("UIWindow"),"alloc"),"init");
        ModRect bounds;CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&bounds,mod_send0(mod_class("UIScreen"),"mainScreen"),mod_sel("bounds"));
        if(bounds.w<300||bounds.h<400)fail("Mod Lab: unsupported screen bounds");mod_frame(mod_window,bounds);
        CALL(mod_msg,void,(u32,u32,float))(mod_window,mod_sel("setWindowLevel:"),10.0f);
        u32 root=mod_send0(mod_send0(mod_class("UIView"),"alloc"),"init");mod_frame(root,bounds);mod_send1(root,"setBackgroundColor:",mod_color(0.06f,0.09f,0.14f));
        u32 vc=mod_send0(mod_send0(mod_class("UIViewController"),"alloc"),"init");mod_send1(vc,"setView:",root);mod_send1(mod_window,"setRootViewController:",vc);
        mod_label(root,"MOD LAB",(ModRect){12,14,180,30},22);mod_button(root,"Close",10,(ModRect){bounds.w-80,12,68,32});
        u32 scroll=mod_send0(mod_send0(mod_class("UIScrollView"),"alloc"),"init");mod_frame(scroll,(ModRect){0,54,bounds.w,bounds.h-110});mod_send1(root,"addSubview:",scroll);mod_send1(scroll,"setClipsToBounds:",1);
#ifdef GUEST_MECH_CARD_COUNT
        mod_main_scroll=scroll;
#endif
        CALL(mod_msg,void,(u32,u32,ModSize))(scroll,mod_sel("setContentSize:"),(ModSize){bounds.w,1010});
        mod_label(scroll,"Account",(ModRect){12,0,240,24},18);
        mod_label(scroll,"Gold",(ModRect){12,32,52,32},14);mod_gold_field=mod_field(scroll,(ModRect){70,32,128,32},0);
        mod_button(scroll,"-100",20,(ModRect){204,32,48,32});mod_button(scroll,"+100",21,(ModRect){258,32,50,32});
        mod_label(scroll,"Gems",(ModRect){12,74,52,32},14);mod_gem_field=mod_field(scroll,(ModRect){70,74,128,32},0);
        mod_button(scroll,"-10",22,(ModRect){204,74,48,32});mod_button(scroll,"+10",23,(ModRect){258,74,50,32});
        mod_button(scroll,"Save balances",24,(ModRect){12,118,296,34});
        mod_label(scroll,"Unit blueprints",(ModRect){12,168,200,26},18);
#ifdef GUEST_SPELL_COUNT
        mod_button(scroll,"Spells",41,(ModRect){220,168,88,30});
#endif
        mod_button(scroll,"<",12,(ModRect){12,204,40,32});mod_unit_label=mod_label(scroll,"",(ModRect){60,204,198,32},17);mod_button(scroll,">",13,(ModRect){268,204,40,32});
        mod_toggle=mod_button(scroll,"",11,(ModRect){12,246,296,32});
        const char *names[MOD_FIELDS]={"Move speed %","Attack speed %","Windup speed %","Troop size %","Shot speed %","Health %","Damage %","Attack range %"};
        for(u32 f=0;f<MOD_FIELDS;++f){float y=290+(float)f*43;mod_label(scroll,names[f],(ModRect){12,y,142,32},14);
            mod_minus[f]=mod_button(scroll,"-",100+(int)f*2,(ModRect){158,y,32,32});mod_fields[f]=mod_field(scroll,(ModRect){196,y,72,32},100);mod_plus[f]=mod_button(scroll,"+",101+(int)f*2,(ModRect){276,y,32,32});}
        mod_button(scroll,"Apply units",14,(ModRect){12,650,144,36});mod_button(scroll,"Reset draft",15,(ModRect){164,650,144,36});
        mod_label(scroll,"Shared units share modifiers.",(ModRect){12,700,300,22},12);
        mod_label(scroll,"Default = 100%. Training stays original.",(ModRect){12,724,300,22},12);
        mod_label(scroll,"A pre-mod save is kept in Documents.",(ModRect){12,748,300,22},12);
        mod_label(scroll,"Clone selected blueprint",(ModRect){12,792,300,26},18);
        forge_name_field=mod_field(scroll,(ModRect){12,828,296,34},0);
        mod_send1(forge_name_field,"setKeyboardType:",0);mod_send1(forge_name_field,"setAutocapitalizationType:",0);
        mod_send1(forge_name_field,"setAutocorrectionType:",1);mod_text(forge_name_field,"");
        mod_send1(forge_name_field,"setPlaceholder:",mod_string("Custom card name"));
#ifdef GUEST_ART_SOURCE_TOKEN
        forge_create_button=mod_button(scroll,"Create card",30,(ModRect){12,878,190,36});forge_art_button=mod_button(scroll,"Images",31,(ModRect){210,878,98,36});
#else
        forge_create_button=mod_button(scroll,"Create custom card",30,(ModRect){12,878,296,36});
#endif
        mod_label(scroll,"Restart Clash to load new cards.",(ModRect){12,926,300,22},12);
        mod_label(scroll,"Definitions are permanent.",(ModRect){12,950,300,22},12);
        mod_status_label=mod_label(root,"",(ModRect){12,bounds.h-52,bounds.w-24,46},12);mod_send1(mod_status_label,"setNumberOfLines:",2);
#ifdef GUEST_MECH_CARD_COUNT
        mech_ui_setup(root,bounds);
#endif
    }
#ifdef GUEST_MECH_CARD_COUNT
    mech_ui_begin();
#endif
    void *home,*avatar;if(mod_home(&home,&avatar)){mod_set_number(mod_gold_field,CALL(0x9ab55,u32,(void*))(avatar));mod_set_number(mod_gem_field,CALL(0x9a41d,u32,(void*))(avatar));}
    mod_message(mod_profile_rejected?"Saved modifiers rejected. Reset draft.":"Edit, then save. Close cancels draft.");mod_ui_refresh(1);mod_send1(mod_window,"setHidden:",0);mod_send0(mod_window,"makeKeyAndVisible");
}
static void mod_tick(void) {
    if(!save_initialized || !logged_in || battle_started)return;
    mod_initialize();if(!mod_applied)mod_apply_units();
#ifdef GUEST_ART_SOURCE_TOKEN
    photo_tick();
#endif
}
extern void *guest_original_mod_string(const char*);
static int mod_equal(const char *a,const char *b){if(!a||!b)return 0;u32 i=0;while(a[i]&&a[i]==b[i])++i;return a[i]==b[i];}
void *guest_mod_string(const char *key) {
    // Preserve the exact user-supplied UTF-8 name, independent of the archived
    // CSV parser's quote-escaping dialect and the selected game language.
    static u32 forge_names[FORGE_MAX_CARDS][5],forge_named[FORGE_MAX_CARDS];
    u32 slot=forge_key_slot(key);
    if(slot<forge_loaded_count) {
        if(!forge_named[slot]){CALL(0x18dcf9,void,(void*,const char*))(forge_names[slot],forge_records[slot].name);forge_named[slot]=1;}
        return forge_names[slot];
    }
    static u32 title[5],open[5];static int ready;
    if(mod_equal(key,"TID_FACEBOOK") || mod_equal(key,"TID_FACEBOOK_DISCONNECTED") || mod_equal(key,"TID_FACEBOOK_CONNECTED")) {
        if(!ready){CALL(0x18dcf9,void,(void*,const char*))(title,"Mod Lab");CALL(0x18dcf9,void,(void*,const char*))(open,"Open editor");ready=1;}
        return mod_equal(key,"TID_FACEBOOK")?(void*)title:(void*)open;
    }
    return guest_original_mod_string(key);
}
