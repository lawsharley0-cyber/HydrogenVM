static u32 mech_scroll,mech_card_label,mech_toggle,mech_labels[MECH_FIELDS],mech_minus[MECH_FIELDS],mech_plus[MECH_FIELDS];
static struct MechRecord mech_draft[MECH_CAPACITY],mech_previous[MECH_CAPACITY];
static u32 mech_draft_enabled,mech_draft_selected,mech_showing;
static u32 mod_main_scroll,mod_mech_button;
static void mech_ui_refresh(void){
    mod_text(mech_card_label,mech_name(mech_draft_selected));mod_title(mech_toggle,mech_draft_enabled?"Mechanics: ON":"Mechanics: OFF");
    for(u32 f=0;f<MECH_FIELDS;++f){u32 v=mech_draft[mech_draft_selected].value[f];char number[12];const char *text;
        int enabled=mech_supported(mech_draft_selected,f);
        if(!enabled)text="N/A";else if(!v)text="Default";
        else if(f==MECH_SPAWN_TYPE || f==MECH_IMPACT_TYPE)text=mech_troop_names[v-1];
        else if(f==MECH_SHOT)text=mech_shot_names[v-1];else{mod_number(number,v);text=number;}
        mod_text(mech_labels[f],text);mod_send1(mech_minus[f],"setEnabled:",enabled);mod_send1(mech_plus[f],"setEnabled:",enabled);
    }
    mod_text(mod_status_label,mod_status);
}
static void mech_ui_begin(void){
    mech_initialize();for(u32 i=0;i<mech_count();++i)mech_draft[i]=mech_records[i];
    mech_draft_enabled=mech_enabled;mech_draft_selected=mech_selected;mech_reset_requested=0;mech_showing=0;
    mod_send1(mod_main_scroll,"setHidden:",0);mod_send1(mech_scroll,"setHidden:",1);mod_title(mod_mech_button,"Spawns");
}
static void mech_ui_action(int tag){
    if(tag==40){mech_showing=!mech_showing;mod_send1(mod_main_scroll,"setHidden:",mech_showing);mod_send1(mech_scroll,"setHidden:",!mech_showing);mod_title(mod_mech_button,mech_showing?"Tuning":"Spawns");
        mod_message(mech_rejected?"Mechanics file rejected. Reset to recover.":"Edits need Apply. Default = original.");mech_ui_refresh();return;}
    if(tag==401)mech_draft_enabled=!mech_draft_enabled;
    else if(tag==402)mech_draft_selected=mech_draft_selected?mech_draft_selected-1:mech_count()-1;
    else if(tag==403)mech_draft_selected=mech_draft_selected+1<mech_count()?mech_draft_selected+1:0;
    else if(tag==405){for(u32 i=0;i<mech_count();++i)for(u32 f=0;f<MECH_FIELDS;++f)mech_draft[i].value[f]=0;mech_draft_enabled=0;mech_reset_requested=1;mod_message("Native defaults staged. Apply to save.");}
    else if(tag==404){
        void *home,*avatar;if(!mod_home(&home,&avatar)){mod_message("Finish pending rewards and return home first.");mech_ui_refresh();return;}
        for(u32 i=0;i<mech_count();++i)if(!mech_record_valid(i,&mech_draft[i])){mod_message("Unsupported mechanics choice. Nothing saved.");mech_ui_refresh();return;}
        if(mech_rejected && !mech_reset_requested){mod_message("Mechanics file rejected. Reset before Apply.");mech_ui_refresh();return;}
        capture_native(home,avatar);mod_backup();
        u32 old_enabled=mech_enabled,old_selected=mech_selected;
        for(u32 i=0;i<mech_count();++i){mech_previous[i]=mech_records[i];mech_records[i]=mech_draft[i];}
        mech_enabled=mech_draft_enabled;mech_selected=mech_draft_selected;
        if(!mech_save()){for(u32 i=0;i<mech_count();++i)mech_records[i]=mech_previous[i];mech_enabled=old_enabled;mech_selected=old_selected;}
    } else if(tag>=1000 && tag<1000+MECH_FIELDS*2){
        u32 f=(u32)(tag-1000)/2;if(!mech_supported(mech_draft_selected,f))return;
        u32 *v=&mech_draft[mech_draft_selected].value[f],step=1,limit=10,min=1;
        if(f==MECH_COUNT)limit=16;else if(f==MECH_SPAWN_COUNT||f==MECH_IMPACT_COUNT)limit=8;
        else if(f==MECH_SPAWN_TYPE||f==MECH_IMPACT_TYPE)limit=GUEST_MECH_TROOP_COUNT;
        else if(f==MECH_SHOT)limit=GUEST_MECH_SHOT_COUNT;
        else if(f==MECH_PAUSE){min=500;step=500;limit=60000;}else if(f==MECH_DELAY){min=100;step=100;limit=10000;}
        if(tag&1)*v=!*v?min:*v>=limit?0:*v+step;else *v=!*v?limit:*v<=min?0:*v-step;
    }
    mech_ui_refresh();
}
static void mech_ui_setup(u32 root,ModRect bounds){
    mod_mech_button=mod_button(root,"Spawns",40,(ModRect){164,12,72,32});
    mech_scroll=mod_send0(mod_send0(mod_class("UIScrollView"),"alloc"),"init");mod_frame(mech_scroll,(ModRect){0,54,bounds.w,bounds.h-110});mod_send1(root,"addSubview:",mech_scroll);mod_send1(mech_scroll,"setClipsToBounds:",1);
    CALL(mod_msg,void,(u32,u32,ModSize))(mech_scroll,mod_sel("setContentSize:"),(ModSize){bounds.w,710});
    mod_button(mech_scroll,"<",402,(ModRect){12,8,36,32});mech_card_label=mod_label(mech_scroll,"",(ModRect){54,8,208,32},16);mod_button(mech_scroll,">",403,(ModRect){272,8,36,32});
    mech_toggle=mod_button(mech_scroll,"",401,(ModRect){12,50,296,32});
    const char *labels[MECH_FIELDS]={"Troops / card","Summon count","Summon troop","Projectile","Impact troops","Impact troop","Spawn pause ms","Deploy delay ms","Elixir cost"};
    for(u32 f=0;f<MECH_FIELDS;++f){float y=98+(float)f*43;mod_label(mech_scroll,labels[f],(ModRect){12,y,126,32},13);
        mech_minus[f]=mod_button(mech_scroll,"-",1000+(int)f*2,(ModRect){140,y,28,32});
        mech_labels[f]=mod_label(mech_scroll,"",(ModRect){172,y,102,32},12);mod_send1(mech_labels[f],"setNumberOfLines:",2);mod_send1(mech_labels[f],"setTextAlignment:",1);
        mech_plus[f]=mod_button(mech_scroll,"+",1001+(int)f*2,(ModRect){280,y,28,32});}
    mod_button(mech_scroll,"Apply spawns",404,(ModRect){12,500,144,36});mod_button(mech_scroll,"Reset spawns",405,(ModRect){164,500,144,36});
    mod_label(mech_scroll,"Default restores the original value.",(ModRect){12,552,296,24},12);
    mod_label(mech_scroll,"Shared troops/projectiles share edits.",(ModRect){12,580,296,24},12);
    mod_label(mech_scroll,"Replacement summons start at Level 1.",(ModRect){12,608,296,24},12);
    mod_label(mech_scroll,"Training and friendly battles stay original.",(ModRect){12,636,296,24},12);
    mod_send1(mech_scroll,"setHidden:",1);
}
