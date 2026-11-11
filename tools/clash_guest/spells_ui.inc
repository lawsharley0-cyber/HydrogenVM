static u32 spell_panel,spell_label,spell_toggle,spell_labels[SPELL_FIELDS],spell_minus[SPELL_FIELDS],spell_plus[SPELL_FIELDS];
static struct SpellRecord spell_draft[GUEST_SPELL_COUNT],spell_previous[GUEST_SPELL_COUNT];
static u32 spell_draft_enabled,spell_draft_selected;
static void spell_ui_refresh(void){
    mod_text(spell_label,spell_defs[spell_draft_selected].name);mod_title(spell_toggle,spell_draft_enabled?"Spells: ON":"Spells: OFF");
    for(u32 f=0;f<SPELL_FIELDS;++f){int supported=(spell_defs[spell_draft_selected].caps&(1u<<f))!=0;u32 value=spell_draft[spell_draft_selected].value[f];char n[12];
        mod_number(n,value);mod_text(spell_labels[f],!supported?"N/A":f==SPELL_COST && !value?"Default":n);
        mod_send1(spell_minus[f],"setEnabled:",supported);mod_send1(spell_plus[f],"setEnabled:",supported);}
    mod_text(mod_status_label,mod_status);
}
static void spell_ui_open(void){
    spell_initialize();spell_reset_requested=0;spell_draft_enabled=spell_enabled;spell_draft_selected=spell_selected;
    for(u32 i=0;i<GUEST_SPELL_COUNT;++i)spell_draft[i]=spell_records[i];
    if(!spell_panel){u32 root=mod_send0(mod_send0(mod_window,"rootViewController"),"view");spell_panel=mod_send0(mod_send0(mod_class("UIScrollView"),"alloc"),"init");
        ModRect bounds;CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&bounds,mod_send0(mod_class("UIScreen"),"mainScreen"),mod_sel("bounds"));
        mod_frame(spell_panel,(ModRect){0,54,bounds.w,bounds.h-110});mod_send1(spell_panel,"setBackgroundColor:",mod_color(.06f,.09f,.14f));mod_send1(root,"addSubview:",spell_panel);mod_send1(spell_panel,"setClipsToBounds:",1);
        CALL(mod_msg,void,(u32,u32,ModSize))(spell_panel,mod_sel("setContentSize:"),(ModSize){320,650});
        mod_button(spell_panel,"<",340,(ModRect){12,8,36,32});spell_label=mod_label(spell_panel,"",(ModRect){54,8,208,32},17);mod_button(spell_panel,">",341,(ModRect){272,8,36,32});
        spell_toggle=mod_button(spell_panel,"",342,(ModRect){12,50,296,32});
        const char *names[SPELL_FIELDS]={"Damage %","Radius %","Area duration %","Flight speed %","Elixir cost"};
        for(u32 f=0;f<SPELL_FIELDS;++f){float y=98+(float)f*43;mod_label(spell_panel,names[f],(ModRect){12,y,126,32},13);
            spell_minus[f]=mod_button(spell_panel,"-",300+(int)f*2,(ModRect){140,y,28,32});spell_labels[f]=mod_label(spell_panel,"",(ModRect){178,y,90,32},15);
            spell_plus[f]=mod_button(spell_panel,"+",301+(int)f*2,(ModRect){280,y,28,32});}
        mod_button(spell_panel,"Apply spells",343,(ModRect){12,330,144,36});mod_button(spell_panel,"Reset spells",344,(ModRect){164,330,144,36});
        mod_label(spell_panel,"100% / Default = no spell override.",(ModRect){12,380,300,24},12);
        mod_label(spell_panel,"Shared effects share changes.",(ModRect){12,410,300,24},12);
        mod_label(spell_panel,"Spell overrides take priority when set.",(ModRect){12,440,300,24},12);
        mod_label(spell_panel,"N/A fields keep their native behavior.",(ModRect){12,470,300,24},12);
        mod_button(spell_panel,"Back",345,(ModRect){12,510,296,36});
    }
    mod_message(spell_rejected?"Spell file rejected. Reset to recover.":"Apply saves. Training stays original.");mod_send1(spell_panel,"setHidden:",0);spell_ui_refresh();
}
static void spell_ui_action(int tag){
    if(tag==345){mod_send1(spell_panel,"setHidden:",1);return;}
    if(tag==340)spell_draft_selected=spell_draft_selected?spell_draft_selected-1:GUEST_SPELL_COUNT-1;
    else if(tag==341)spell_draft_selected=spell_draft_selected+1<GUEST_SPELL_COUNT?spell_draft_selected+1:0;
    else if(tag==342)spell_draft_enabled=!spell_draft_enabled;
    else if(tag==344){for(u32 i=0;i<GUEST_SPELL_COUNT;++i)for(u32 f=0;f<SPELL_FIELDS;++f)spell_draft[i].value[f]=spell_default(f);spell_draft_enabled=0;spell_reset_requested=1;mod_message("Spell defaults staged. Apply to save.");}
    else if(tag==343){
        void *home,*avatar;if(!mod_home(&home,&avatar)){mod_message("Finish rewards and return home first.");spell_ui_refresh();return;}
        for(u32 i=0;i<GUEST_SPELL_COUNT;++i)if(!spell_record_valid(i,&spell_draft[i])){mod_message("Invalid spell draft. Nothing saved.");spell_ui_refresh();return;}
        capture_native(home,avatar);mod_backup();u32 enabled=spell_enabled,selected=spell_selected;
        for(u32 i=0;i<GUEST_SPELL_COUNT;++i){spell_previous[i]=spell_records[i];spell_records[i]=spell_draft[i];}spell_enabled=spell_draft_enabled;spell_selected=spell_draft_selected;
        if(!spell_save()){for(u32 i=0;i<GUEST_SPELL_COUNT;++i)spell_records[i]=spell_previous[i];spell_enabled=enabled;spell_selected=selected;}
    }else if(tag>=300 && tag<300+SPELL_FIELDS*2){u32 f=(u32)(tag-300)/2;
        if(!(spell_defs[spell_draft_selected].caps&(1u<<f)))return;u32 *value=&spell_draft[spell_draft_selected].value[f];
        if(f==SPELL_COST){if(tag&1)*value=*value==10?0:*value+1;else *value=*value?*value-1:10;}
        else{u32 low=f==SPELL_DAMAGE?10:25,high=f==SPELL_DAMAGE?800:400;if(tag&1)*value=*value+25>high?high:*value+25;else *value=*value<low+25?low:*value-25;}
    }
    spell_ui_refresh();
}
