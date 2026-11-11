static u32 music_panel,music_picker,music_player,music_candidate_bytes,music_candidate_hash;
static char music_candidate[560];
static void music_status(const char *text){mod_message(text);mod_text(mod_status_label,mod_status);}
static void music_stop(void){
    if(music_player){mod_send0(music_player,"stop");mod_send0(music_player,"release");music_player=0;}
}
static int music_write_header(u32 generation,u32 bytes,u32 hash){
    char path[560],temporary[570];path_append(path,music_folder,"/menu.bin",sizeof path);path_append(temporary,path,".tmp",sizeof temporary);
    if(CALL(symbol("mkdir"),int,(const char*,int))(music_folder,0700) && *CALL(symbol("__error"),int*,(void))()!=17)return 0;
    u32 h[5]={0x3143554du,generation,bytes,hash,0};h[4]=checksum((u8*)h,16);
    void *f=CALL(symbol("fopen"),void*,(const char*,const char*))(temporary,"wb");if(!f)return 0;
    int ok=CALL(symbol("fwrite"),u32,(const void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h;
    int flushed=CALL(symbol("fflush"),int,(void*))(f),fd=CALL(symbol("fileno"),int,(void*))(f);
    ok=ok && !flushed && fd>=0 && !CALL(symbol("fsync"),int,(int))(fd);
    int closed=CALL(symbol("fclose"),int,(void*))(f);
    if(!ok || closed || CALL(symbol("rename"),int,(const char*,const char*))(temporary,path))return 0;
    music_generation=generation;music_bytes=bytes;music_hash=hash;
    if(generation)music_path(music_file,generation);
    return 1;
}
static int music_validate(const char *path,u32 *bytes,u32 *hash){
    void *f=CALL(symbol("fopen"),void*,(const char*,const char*))(path,"rb");if(!f)return 0;
    u8 data[4096];u32 size=0,value=2166136261u;int ok=1,first=1;
    for(;;){
        u32 n=CALL(symbol("fread"),u32,(void*,u32,u32,void*))(data,1,sizeof data,f);if(!n)break;
        if(first){
            first=0;
            int wav=n>=12 && data[0]=='R' && data[1]=='I' && data[2]=='F' && data[3]=='F' && data[8]=='W' && data[9]=='A' && data[10]=='V' && data[11]=='E';
            int mp3=n>=3 && ((data[0]=='I' && data[1]=='D' && data[2]=='3') || (data[0]==255 && (data[1]&0xe0)==0xe0));
            if(!wav && !mp3){ok=0;break;}
        }
        if(n>8388608u-size){ok=0;break;}size+=n;
        for(u32 i=0;i<n;++i)value=(value^data[i])*16777619u;
    }
    ok=ok && size && !CALL(symbol("ferror"),int,(void*))(f);
    int closed=CALL(symbol("fclose"),int,(void*))(f);
    if(!ok || closed)return 0;*bytes=size;*hash=value;return 1;
}
static void music_dismiss(u32 picker){
    CALL(mod_msg,void,(u32,u32,u32,u32))(picker,mod_sel("dismissViewControllerAnimated:completion:"),1,0);
}
static void music_cancelled(u32 self,u32 selector,u32 picker){
    (void)self;(void)selector;music_status("Import cancelled. Current track kept.");music_dismiss(picker);
}
static void music_picked(u32 self,u32 selector,u32 picker,u32 url){
    (void)self;(void)selector;
    u32 path=mod_send0(url,"path");const char *text=path?(const char*)mod_send0(path,"UTF8String"):0;
    u32 bytes,hash;
    if(!text || forge_length(text)>=sizeof music_candidate || !music_validate(text,&bytes,&hash) || !music_duration_bound(text,bytes)){
        music_status("Use PCM WAV / MP3 within the limits.");music_dismiss(picker);return;
    }
    u32 error=0,player=CALL(mod_msg,u32,(u32,u32,u32,u32*))(mod_send0(mod_class("AVAudioPlayer"),"alloc"),mod_sel("initWithContentsOfURL:error:"),url,&error);
    double duration=player?CALL(mod_msg,double,(u32,u32))(player,mod_sel("duration")):0;
    if(error || duration<=0 || duration>180 || !mod_send0(player,"prepareToPlay")){
        if(player)mod_send0(player,"release");
        music_status("Use a playable track up to 3 minutes.");music_dismiss(picker);return;
    }
    music_stop();music_player=player;
    path_append(music_candidate,text,"",sizeof music_candidate);
    music_candidate_bytes=bytes;music_candidate_hash=hash;
    music_status("Track ready. Preview or Use for menu.");music_dismiss(picker);
}
static void music_open(void){
    music_initialize();
    if(!music_panel){
        u32 root=mod_send0(mod_send0(mod_window,"rootViewController"),"view");
        music_panel=mod_send0(mod_send0(mod_class("UIScrollView"),"alloc"),"init");
        ModRect bounds;CALL(symbol("objc_msgSend_stret"),void,(void*,u32,u32))(&bounds,mod_send0(mod_class("UIScreen"),"mainScreen"),mod_sel("bounds"));
        mod_frame(music_panel,(ModRect){0,54,bounds.w,bounds.h-110});mod_send1(root,"addSubview:",music_panel);
        mod_send1(music_panel,"setBackgroundColor:",mod_color(.06f,.09f,.14f));mod_send1(music_panel,"setClipsToBounds:",1);
        CALL(mod_msg,void,(u32,u32,ModSize))(music_panel,mod_sel("setContentSize:"),(ModSize){320,410});
        mod_label(music_panel,"Menu soundtrack",(ModRect){12,4,296,30},20);
        mod_button(music_panel,"Import from Files",380,(ModRect){12,48,296,46});
        mod_button(music_panel,"Preview",381,(ModRect){12,106,144,42});
        mod_button(music_panel,"Stop",382,(ModRect){164,106,144,42});
        mod_button(music_panel,"Use for menu",383,(ModRect){12,166,296,46});
        mod_button(music_panel,"Restore original music",384,(ModRect){12,226,296,46});
        mod_label(music_panel,"PCM WAV / MP3. 8 MB / 3 minutes.",(ModRect){12,292,296,28},12);
        mod_label(music_panel,"Restart Clash after changing music.",(ModRect){12,326,296,28},12);
    }
    mod_send1(music_panel,"setHidden:",0);
    music_status(music_generation?"Custom menu track selected.":"Original menu soundtrack selected.");
}
static void music_action(int tag){
    if(tag==380){
        u32 cls=mod_class("UIDocumentPickerViewController");
        if(!cls){music_status("Update Hydrogen to import from Files.");return;}
        u32 types[2]={mod_string("com.microsoft.waveform-audio"),mod_string("public.mp3")};
        u32 array=CALL(mod_msg,u32,(u32,u32,u32*,u32))(mod_class("NSArray"),mod_sel("arrayWithObjects:count:"),types,2);
        music_picker=CALL(mod_msg,u32,(u32,u32,u32,u32))(mod_send0(cls,"alloc"),mod_sel("initWithDocumentTypes:inMode:"),array,0);
        mod_send1(music_picker,"setDelegate:",mod_target);
        CALL(mod_msg,void,(u32,u32,u32,u32,u32))(mod_send0(mod_window,"rootViewController"),mod_sel("presentViewController:animated:completion:"),music_picker,1,0);
        mod_send0(music_picker,"release");
    }else if(tag==381){
        if(!music_player && music_generation){
            u32 url=mod_send1(mod_class("NSURL"),"fileURLWithPath:",mod_string(music_file));
            u32 error=0;music_player=CALL(mod_msg,u32,(u32,u32,u32,u32*))(mod_send0(mod_class("AVAudioPlayer"),"alloc"),mod_sel("initWithContentsOfURL:error:"),url,&error);
        }
        if(!music_player || !mod_send0(music_player,"play"))music_status("Import a playable track first.");
        else music_status("Playing preview.");
    }else if(tag==382){if(music_player)mod_send0(music_player,"stop");music_status("Preview stopped.");}
    else if(tag==384){
        if(music_write_header(0,0,0)){music_stop();music_candidate[0]=0;music_status("Original selected. Restart Clash.");}
        else music_status("Could not save. Current track kept.");
    }else if(tag==383){
        if(!music_candidate[0] || !music_check(music_candidate,music_candidate_bytes,music_candidate_hash)){music_status("Import a track first.");return;}
        if(CALL(symbol("mkdir"),int,(const char*,int))(music_folder,0700) && *CALL(symbol("__error"),int*,(void))()!=17){music_status("Cannot create music folder.");return;}
        char dest[560];u32 generation=music_generation;void *out=0;
        for(u32 attempt=0;attempt<4096;++attempt){
            ++generation;if(!generation)break;music_path(dest,generation);
            out=CALL(symbol("fopen"),void*,(const char*,const char*))(dest,"wbx");
            if(out || *CALL(symbol("__error"),int*,(void))()!=17)break;
        }
        if(!out){music_status("Cannot store imported track.");return;}
        void *in=CALL(symbol("fopen"),void*,(const char*,const char*))(music_candidate,"rb");
        int ok=in!=0;u8 data[4096];u32 left=music_candidate_bytes;
        while(ok && left){u32 n=left<sizeof data?left:sizeof data;
            ok=CALL(symbol("fread"),u32,(void*,u32,u32,void*))(data,1,n,in)==n && CALL(symbol("fwrite"),u32,(const void*,u32,u32,void*))(data,1,n,out)==n;left-=n;}
        if(in)ok=!CALL(symbol("fclose"),int,(void*))(in) && ok;
        int flushed=CALL(symbol("fflush"),int,(void*))(out),fd=CALL(symbol("fileno"),int,(void*))(out);
        ok=ok && !flushed && fd>=0 && !CALL(symbol("fsync"),int,(int))(fd);
        int closed=CALL(symbol("fclose"),int,(void*))(out);
        if(ok && !closed && music_check(dest,music_candidate_bytes,music_candidate_hash) && music_write_header(generation,music_candidate_bytes,music_candidate_hash)){
            music_status("Menu track saved. Restart Clash.");
        }else music_status("Save failed. Current assignment kept.");
    }
}
