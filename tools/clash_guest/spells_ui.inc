static u32 spell_panel,spell_label,spell_toggle,spell_labels[SPELL_FIELDS],spell_minus[SPELL_FIELDS],spell_plus[SPELL_FIELDS];
static struct SpellRecord spell_draft[SPELL_CAPACITY],spell_previous[SPELL_CAPACITY];
static u32 spell_draft_enabled,spell_draft_selected;
static struct SpellRecord spell_clip,spell_undo[SPELL_CAPACITY];
static u32 spell_clip_caps,spell_has_undo[SPELL_CAPACITY],spell_paste_button,spell_undo_button;
static u32 spell_session_started,spell_locks[SPELL_CAPACITY],spell_lock_buttons[SPELL_FIELDS];
static u32 spell_random_state,spell_shuffle_button,spell_wide_button,spell_heavy_button;
static const char *spell_field_names[SPELL_FIELDS]={"Damage","Radius","Duration","Speed","Cost"};
#ifdef GUEST_FORGE_SPELLS
static u32 spell_clone_button,spell_icon_button,spell_clone_panel,spell_clone_name;
static void spell_clone_open(void){
    u32 root=mod_send0(mod_send0(mod_window,"rootViewController"),"view");
    if(!spell_clone_panel){
        spell_clone_panel=mod_send0(mod_send0(mod_class("UIView"),"alloc"),"init");
        ModRect frame;CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&frame,spell_panel,mod_sel("frame"));
        mod_frame(spell_clone_panel,frame);mod_send1(root,"addSubview:",spell_clone_panel);
        mod_send1(spell_clone_panel,"setBackgroundColor:",mod_color(.06f,.09f,.14f));
        mod_label(spell_clone_panel,"Name your spell",(ModRect){12,8,296,32},18);
        spell_clone_name=mod_field(spell_clone_panel,(ModRect){12,52,296,38},0);
        mod_send1(spell_clone_name,"setKeyboardType:",0);
        mod_button(spell_clone_panel,"Create spell",369,(ModRect){12,106,144,42});
        mod_button(spell_clone_panel,"Cancel",370,(ModRect){164,106,144,42});
        mod_label(spell_clone_panel,"Restart to collect and tune your spell.",(ModRect){12,160,296,38},12);
    }
    mod_text(spell_clone_name,spell_catalog[spell_draft_selected].id==28000000u?"Custom Fireball":"Custom Rocket");
    mod_send1(spell_clone_panel,"setHidden:",0);
}
static void spell_clone_close(void){
    if(spell_clone_panel){mod_send1(spell_clone_panel,"endEditing:",1);mod_send1(spell_clone_panel,"setHidden:",1);}
}
#endif
static void spell_remember(void){
    spell_undo[spell_draft_selected]=spell_draft[spell_draft_selected];
    spell_has_undo[spell_draft_selected]=1;
}
static void spell_ui_refresh(void){
    mod_text(spell_label,spell_catalog[spell_draft_selected].name);mod_title(spell_toggle,spell_draft_enabled?"Spells: ON":"Spells: OFF");
    for(u32 f=0;f<SPELL_FIELDS;++f){int supported=(spell_catalog[spell_draft_selected].caps&(1u<<f))!=0;u32 value=spell_draft[spell_draft_selected].value[f];char n[12];
        mod_number(n,value);mod_text(spell_labels[f],!supported?"N/A":f==SPELL_COST && !value?"Default":n);
        mod_send1(spell_minus[f],"setEnabled:",supported);mod_send1(spell_plus[f],"setEnabled:",supported);}
    u32 caps=spell_catalog[spell_draft_selected].caps;
    for(u32 f=0;f<SPELL_FIELDS;++f){
        u32 button=spell_lock_buttons[f];
        int locked=(spell_locks[spell_draft_selected]&(1u<<f))!=0;
        static const char *locked_names[SPELL_FIELDS]={"[Damage]","[Radius]","[Duration]","[Speed]","[Cost]"};
        mod_title(button,locked?locked_names[f]:spell_field_names[f]);
        mod_send1(button,"setBackgroundColor:",locked?mod_color(.27f,.17f,.31f):mod_color(.14f,.30f,.50f));
        mod_send1(button,"setEnabled:",(caps&(1u<<f))!=0);
    }
#ifdef GUEST_FORGE_SPELLS
#ifdef GUEST_ART_SOURCE_TOKEN
    mod_send1(spell_icon_button,"setEnabled:",spell_draft_selected>=GUEST_SPELL_COUNT);
#endif
    mod_send1(spell_clone_button,"setEnabled:",forge_blueprint(spell_catalog[spell_draft_selected].id)!=0);
#endif
    mod_send1(spell_wide_button,"setEnabled:",(caps&(1u<<SPELL_RADIUS))!=0);
    mod_send1(spell_heavy_button,"setEnabled:",(caps&(1u<<SPELL_DAMAGE))!=0);
    mod_send1(spell_shuffle_button,"setEnabled:",(caps&~spell_locks[spell_draft_selected])!=0);
    mod_send1(spell_paste_button,"setEnabled:",(spell_clip_caps & spell_catalog[spell_draft_selected].caps)!=0);
    mod_send1(spell_undo_button,"setEnabled:",spell_has_undo[spell_draft_selected]);
    mod_text(mod_status_label,mod_status);
}
static void spell_ui_open(void){
    spell_initialize();if(!spell_session_started){spell_session_started=1;spell_reset_requested=0;spell_draft_enabled=spell_enabled;spell_draft_selected=spell_selected;
    for(u32 i=0;i<spell_count();++i){spell_draft[i]=spell_records[i];spell_has_undo[i]=0;spell_locks[i]=0;}
    }
    if(!spell_panel){u32 root=mod_send0(mod_send0(mod_window,"rootViewController"),"view");spell_panel=mod_send0(mod_send0(mod_class("UIScrollView"),"alloc"),"init");
        ModRect bounds;CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&bounds,mod_send0(mod_class("UIScreen"),"mainScreen"),mod_sel("bounds"));
        mod_frame(spell_panel,(ModRect){0,54,bounds.w,bounds.h-110});mod_send1(spell_panel,"setBackgroundColor:",mod_color(.06f,.09f,.14f));mod_send1(root,"addSubview:",spell_panel);mod_send1(spell_panel,"setClipsToBounds:",1);
        CALL(mod_msg,void,(u32,u32,ModSize))(spell_panel,mod_sel("setContentSize:"),(ModSize){320,792});
        mod_button(spell_panel,"<",340,(ModRect){12,8,36,32});spell_label=mod_label(spell_panel,"",(ModRect){54,8,208,32},17);mod_button(spell_panel,">",341,(ModRect){272,8,36,32});
        spell_toggle=mod_button(spell_panel,"",342,(ModRect){12,50,172,36});
        mod_button(spell_panel,"Tools",366,(ModRect){192,50,116,36});
        const char *names[SPELL_FIELDS]={"Damage %","Radius %","Area duration %","Flight speed %","Elixir cost"};
        for(u32 f=0;f<SPELL_FIELDS;++f){float y=98+(float)f*43;mod_label(spell_panel,names[f],(ModRect){12,y,126,32},13);
            spell_minus[f]=mod_button(spell_panel,"-",300+(int)f*2,(ModRect){140,y,28,32});spell_labels[f]=mod_label(spell_panel,"",(ModRect){178,y,90,32},15);
            spell_plus[f]=mod_button(spell_panel,"+",301+(int)f*2,(ModRect){280,y,28,32});}
        mod_button(spell_panel,"Apply spells",343,(ModRect){12,330,144,36});mod_button(spell_panel,"Reset spells",344,(ModRect){164,330,144,36});
        mod_button(spell_panel,"Copy values",346,(ModRect){12,375,144,40});
        spell_paste_button=mod_button(spell_panel,"Paste values",347,(ModRect){164,375,144,40});
        spell_wide_button=mod_button(spell_panel,"Wide area",348,(ModRect){12,425,144,40});
        spell_heavy_button=mod_button(spell_panel,"Heavy hit",349,(ModRect){164,425,144,40});
        mod_button(spell_panel,"Reset one",350,(ModRect){12,475,144,40});
        spell_undo_button=mod_button(spell_panel,"Undo edit",351,(ModRect){164,475,144,40});
        mod_label(spell_panel,"Templates change the draft.",(ModRect){12,522,300,26},12);
#ifdef GUEST_FORGE_SPELLS
        spell_clone_button=mod_button(spell_panel,"Clone base spell",368,(ModRect){12,538,164,36});
#ifdef GUEST_ART_SOURCE_TOKEN
        spell_icon_button=mod_button(spell_panel,"Icon photo",371,(ModRect){184,538,124,36});
#endif
#else
        mod_label(spell_panel,"Shared projectiles share battle effects.",(ModRect){12,550,300,26},12);
#endif
        mod_label(spell_panel,"Lock fields before shuffle",(ModRect){12,580,296,28},15);
        for(u32 f=0;f<SPELL_FIELDS;++f){
            float left=12+100*(float)(f<3?f:f-3), top=f<3?614:660;
            spell_lock_buttons[f]=mod_button(spell_panel,spell_field_names[f],360+(int)f,(ModRect){left,top,94,40});
        }
        spell_shuffle_button=mod_button(spell_panel,"Shuffle",365,(ModRect){212,660,94,40});
        mod_button(spell_panel,"Back to values",367,(ModRect){12,722,296,40});
    }
    mod_message(spell_rejected?"Spell file rejected. Reset to recover.":"Apply saves. Training stays original.");mod_send1(spell_panel,"setHidden:",0);spell_ui_refresh();
}
static void spell_ui_action(int tag){
#ifdef GUEST_FORGE_SPELLS
#ifdef GUEST_ART_SOURCE_TOKEN
    if(tag==371 && spell_draft_selected>=GUEST_SPELL_COUNT){
        photo_choose_slot(forge_spell_slots[spell_draft_selected-GUEST_SPELL_COUNT],0);return;
    }
#endif
    if(tag==368){spell_clone_open();return;}
    if(tag==370){spell_clone_close();return;}
    if(tag==369){
        void *home,*avatar;
        const char *name=(const char*)mod_send0(mod_send0(spell_clone_name,"text"),"UTF8String");
        u32 bytes=0;if(name)while(bytes<=96 && name[bytes])++bytes;
        if(!name || !forge_name_valid(name,bytes))mod_message("Enter a name of 1-32 characters.");
        else if(forge_count>=FORGE_MAX_CARDS)mod_message("Maximum 16 custom cards per profile.");
        else if(!mod_home(&home,&avatar))mod_message("Finish pending rewards first.");
        else{
            capture_native(home,avatar);mod_backup();
            if(forge_append(spell_catalog[spell_draft_selected].id,name)){
                spell_clone_close();mod_message("Created. Restart to collect the spell.");
            }else mod_message("Could not save the new spell.");
        }
        spell_ui_refresh();return;
    }
#endif
    if(tag==366 || tag==367){
        ModRect frame;
        CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&frame,spell_panel,mod_sel("bounds"));
        float bottom=792-frame.h;
        CALL(mod_msg,void,(u32,u32,ModSize))(spell_panel,mod_sel("setContentOffset:"),(ModSize){0,tag==366 && bottom>0?bottom:0});
        return;
    }
    if(tag==340)spell_draft_selected=spell_draft_selected?spell_draft_selected-1:spell_count()-1;
    else if(tag==341)spell_draft_selected=spell_draft_selected+1<spell_count()?spell_draft_selected+1:0;
    else if(tag==342)spell_draft_enabled=!spell_draft_enabled;
    else if(tag==344){for(u32 i=0;i<spell_count();++i)spell_has_undo[i]=0;for(u32 i=0;i<spell_count();++i)for(u32 f=0;f<SPELL_FIELDS;++f)spell_draft[i].value[f]=spell_default(f);spell_draft_enabled=0;spell_reset_requested=1;mod_message("Spell defaults staged. Apply to save.");}
    else if(tag==343){
        void *home,*avatar;if(!mod_home(&home,&avatar)){mod_message("Finish rewards and return home first.");spell_ui_refresh();return;}
        for(u32 i=0;i<spell_count();++i)if(!spell_record_valid(i,&spell_draft[i])){mod_message("Invalid spell draft. Nothing saved.");spell_ui_refresh();return;}
        capture_native(home,avatar);mod_backup();u32 enabled=spell_enabled,selected=spell_selected;
        for(u32 i=0;i<spell_count();++i){spell_previous[i]=spell_records[i];spell_records[i]=spell_draft[i];}spell_enabled=spell_draft_enabled;spell_selected=spell_draft_selected;
        if(!spell_save()){for(u32 i=0;i<spell_count();++i)spell_records[i]=spell_previous[i];spell_enabled=enabled;spell_selected=selected;}
    }else if(tag>=346 && tag<=351){
        u32 caps=spell_catalog[spell_draft_selected].caps;
        struct SpellRecord *draft=&spell_draft[spell_draft_selected];
        if(tag==346){spell_clip=*draft;spell_clip_caps=caps;mod_message("Copied. Pick a spell and paste.");}
        else if(tag==351 && spell_has_undo[spell_draft_selected]){
            *draft=spell_undo[spell_draft_selected];spell_has_undo[spell_draft_selected]=0;
            mod_message("Edit undone. Apply to save.");
        }else if(tag>=347 && tag<=350){
            u32 mask=tag==347?spell_clip_caps:tag==348?(1u<<SPELL_RADIUS):tag==349?(1u<<SPELL_DAMAGE):caps;
            mask &= caps;
            if(mask){
                spell_remember();
                for(u32 f=0;f<SPELL_FIELDS;++f)if(mask&(1u<<f))
                    draft->value[f]=tag==347?spell_clip.value[f]:tag==350?spell_default(f):150;
                mod_message("Values staged. Apply to save.");
            }else mod_message("This spell has no matching fields.");
        }
    }else if(tag>=360 && tag<365){
        u32 bit=1u<<(tag-360);
        if(spell_catalog[spell_draft_selected].caps&bit)spell_locks[spell_draft_selected]^=bit;
    }else if(tag==365){
        u32 fields=spell_catalog[spell_draft_selected].caps&~spell_locks[spell_draft_selected];
        if(fields){
            spell_remember();
            if(!spell_random_state)spell_random_state=wall_epoch();
            for(u32 f=0;f<SPELL_FIELDS;++f)if(fields&(1u<<f)){
                u32 roll=opponent_random(&spell_random_state);
                spell_draft[spell_draft_selected].value[f]=f==SPELL_COST?1+opponent_remainder(roll,10):50+25*opponent_remainder(roll,7);
            }
            mod_message("Shuffle staged. Undo or Apply.");
        }
    }else if(tag>=300 && tag<300+SPELL_FIELDS*2){u32 f=(u32)(tag-300)/2;
        if(!(spell_catalog[spell_draft_selected].caps&(1u<<f)))return;spell_remember();u32 *value=&spell_draft[spell_draft_selected].value[f];
        if(f==SPELL_COST){if(tag&1)*value=*value==10?0:*value+1;else *value=*value?*value-1:10;}
        else{u32 low=f==SPELL_DAMAGE?10:25,high=f==SPELL_DAMAGE?800:400;if(tag&1)*value=*value+25>high?high:*value+25;else *value=*value<low+25?low:*value-25;}
    }
    spell_ui_refresh();
}
