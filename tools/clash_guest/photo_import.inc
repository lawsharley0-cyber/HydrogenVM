#include "photo_assets.inc"
static u32 photo_panel,photo_picker,photo_slot,photo_kind,photo_image,photo_pending,photo_delay;
static void photo_status(const char *s){mod_message(s);mod_text(mod_status_label,mod_status);}
static void photo_path(char out[600],const char *folder,u32 kind,u32 slot){char name[64],suffix[68];art_asset_name(name,kind,slot);path_append(suffix,"/",name,sizeof suffix);path_append(out,folder,suffix,600);}
static int photo_original(char out[600],const char *leaf){
    u32 bundle=mod_send0(mod_class("NSBundle"),"mainBundle"),path=mod_send0(bundle,"bundlePath");const char *root=(const char*)mod_send0(path,"UTF8String");
    if(!root)return 0;char folder[560];path_append(folder,root,"/res/sc",sizeof folder);path_append(out,folder,"/",600);u32 at=forge_length(out),n=forge_length(leaf);if(at+n>=600)return 0;memcpy(out+at,leaf,n+1);return 1;
}
static int photo_publish(const u8 *rgba,u32 slot,u32 kind){
    if(!forge_disk_matches())return 0;
    char root[512];const char *home=CALL(s_getenv,const char*,(const char*))("HOME");path_append(root,home,"/Documents/ClashForgeArt-v1",sizeof root);
    if(CALL(symbol("mkdir"),int,(const char*,int))(root,0700) && *CALL(f_errno,int*,(void))()!=17)return 0;
    char stage[600];u32 generation;
    for(generation=1;generation<4096;++generation){char suffix[16];art_generation(suffix,generation);path_append(stage,root,suffix,sizeof stage);
        if(!CALL(symbol("mkdir"),int,(const char*,int))(stage,0700))break;
        if(*CALL(f_errno,int*,(void))()!=17)return 0;
    }
    if(generation==4096)return 0;
    u32 rows[34][4],count=art_valid?art_file_count:0,changed[34]={0},flags[FORGE_MAX_CARDS]={0};
    for(u32 i=0;i<count;++i)for(u32 j=0;j<4;++j)rows[i][j]=art_files[i][j];
    if(art_valid)for(u32 i=0;i<forge_loaded_count;++i)flags[i]=art_flags[i];
    struct PhotoBytes meta={0,0,0,0},tex={0,0,0,0};int ok=1;
    if(kind){ok=ph_troop(&meta,&tex,slot,rgba);if(ok)ok=ph_save_asset(stage,2,slot,&meta,rows,&count,changed) && ph_save_asset(stage,3,slot,&tex,rows,&count,changed);flags[slot]|=2;}
    else {
        char source[600];int has_icons=0;for(u32 i=0;i<forge_loaded_count;++i)has_icons|=flags[i]&1;
        if(has_icons)photo_path(source,art_folder,0,0);else if(!photo_original(source,"ui_spells.sc"))ok=0;
        if(ok)meta=ph_unpack(source,stage);
        if(has_icons)photo_path(source,art_folder,1,0);else if(!photo_original(source,"ui_spells_tex.sc"))ok=0;
        if(ok)tex=ph_unpack(source,stage);
        if(ok)ok=ph_icon(&meta,&tex,slot,rgba);
        if(ok)ok=ph_save_asset(stage,0,0,&meta,rows,&count,changed) && ph_save_asset(stage,1,0,&tex,rows,&count,changed);flags[slot]|=1;
    }
    ph_free(&meta);ph_free(&tex);if(!ok)return 0;
    for(u32 i=0;i<count;++i)if(!changed[i]){char from[600],to[600];photo_path(from,art_folder,rows[i][0],rows[i][1]);photo_path(to,stage,rows[i][0],rows[i][1]);if(!ph_copy(from,to,0))return 0;}
    u32 body[153],words=1+forge_loaded_count+count*4;body[0]=generation;
    for(u32 i=0;i<forge_loaded_count;++i)body[1+i]=flags[i];
    for(u32 i=0;i<count;++i)for(u32 j=0;j<4;++j)body[1+forge_loaded_count+i*4+j]=rows[i][j];
    u32 header[8]={0x32545241u,2,forge_loaded_count,checksum((const u8*)forge_records,forge_loaded_count*sizeof *forge_records),GUEST_ART_SOURCE_TOKEN,count,checksum((const u8*)body,words*4),0};header[7]=checksum((const u8*)header,28);
    u8 manifest[644];memcpy(manifest,header,32);memcpy(manifest+32,body,words*4);
    char current[600],backup[620],temp[620],snapshot[620];path_append(current,root,"/manifest.bin",sizeof current);path_append(backup,current,".before-photos",sizeof backup);path_append(temp,current,".tmp",sizeof temp);path_append(snapshot,stage,"/manifest.bin",sizeof snapshot);
    void *old=CALL(f_open,void*,(const char*,const char*))(current,"rb");if(old){CALL(f_close,int,(void*))(old);void *prior=CALL(f_open,void*,(const char*,const char*))(backup,"rb");if(prior)CALL(f_close,int,(void*))(prior);else if(*CALL(f_errno,int*,(void))()!=2 || !ph_copy(current,backup,0))return 0;}
    if(!ph_file(snapshot,manifest,32+words*4) || !ph_file(temp,manifest,32+words*4) || CALL(f_rename,int,(const char*,const char*))(temp,current))return 0;
    // Existing references remain valid: edits add/replace only. Refresh the
    // loader snapshot so another selection before restart preserves this one.
    art_ready=art_valid=art_rejected=0;memset(art_flags,0,sizeof art_flags);art_initialize();return art_valid;
}
static u8 *photo_pixels(u32 image,u32 kind){
    u32 cg=mod_send0(image,"CGImage");u32 w=CALL(symbol("CGImageGetWidth"),u32,(u32))(cg),h=CALL(symbol("CGImageGetHeight"),u32,(u32))(cg);if(!w || !h || w>4096 || h>4096)return 0;
    float height=kind?160.0f:162.0f,scale=128.0f/(float)w;if((float)h*scale>height)scale=height/(float)h;
    CALL(symbol("UIGraphicsBeginImageContextWithOptions"),void,(ModSize,int,float))((ModSize){128,height},0,1);
    ModRect fit={(128-(float)w*scale)/2,height-(float)h*scale,(float)w*scale,(float)h*scale};
    CALL(mod_msg,void,(u32,u32,ModRect))(image,mod_sel("drawInRect:"),fit);
    u32 scaled=CALL(symbol("UIGraphicsGetImageFromCurrentImageContext"),u32,(void))(),result_cg=mod_send0(scaled,"CGImage");
    u32 provider=CALL(symbol("CGImageGetDataProvider"),u32,(u32))(result_cg),data=CALL(symbol("CGDataProviderCopyData"),u32,(u32))(provider);
    u32 n=128*(kind?160u:162u)*4;u8 *copy=0;
    if(data && CALL(symbol("CFDataGetLength"),u32,(u32))(data)==n){copy=CALL(API_malloc,u8*,(u32))(n);if(copy)memcpy(copy,CALL(symbol("CFDataGetBytePtr"),const void*,(u32))(data),n);}
    if(data)CALL(symbol("CFRelease"),void,(u32))(data);CALL(symbol("UIGraphicsEndImageContext"),void,(void))();return copy;
}
static void photo_tick(void){
    if(!photo_pending)return;if(photo_delay){--photo_delay;return;}photo_pending=0;
    void *home,*avatar;int ok=mod_home(&home,&avatar);u8 *pixels=ok?photo_pixels(photo_image,photo_kind):0;
    if(pixels){ok=photo_publish(pixels,photo_slot,photo_kind);CALL(f_free,void,(void*))(pixels);}else ok=0;
    mod_send0(photo_image,"release");photo_image=0;
    photo_status(ok?"Photo saved. Restart Clash to apply.":"Photo could not be saved. Previous art kept.");
}
static void photo_dismiss(u32 picker){CALL(mod_msg,void,(u32,u32,u32,u32))(picker,mod_sel("dismissViewControllerAnimated:completion:"),1,0);mod_send0(picker,"release");photo_picker=0;}
static void photo_finished(u32 self,u32 selector,u32 picker,u32 info){
    (void)self;(void)selector;u32 image=mod_send1(info,"objectForKey:",mod_string("UIImagePickerControllerOriginalImage"));
    if(image && !photo_pending){photo_image=mod_send0(image,"retain");photo_pending=1;photo_delay=4;photo_status("Preparing photo. Please wait...");}
    else photo_status("No image selected. Previous art kept.");photo_dismiss(picker);
}
static void photo_cancelled(u32 self,u32 selector,u32 picker){(void)self;(void)selector;photo_status("Photo selection cancelled. Art kept.");photo_dismiss(picker);}
static void photo_choose(u32 kind){
    if(photo_pending){photo_status("Preparing the previous photo...");return;}
    void *home,*avatar;if(!mod_home(&home,&avatar)){photo_status("Return home and finish pending rewards.");return;}
    if(!mod_send1(mod_class("UIImagePickerController"),"isSourceTypeAvailable:",0)){photo_status("Update Hydrogen to enable Photos.");return;}
    photo_slot=mod_selected-GUEST_MOD_UNIT_COUNT;photo_kind=kind;
    photo_picker=mod_send0(mod_send0(mod_class("UIImagePickerController"),"alloc"),"init");mod_send1(photo_picker,"setSourceType:",0);mod_send1(photo_picker,"setDelegate:",mod_target);mod_send1(photo_picker,"setAllowsEditing:",0);
    u32 presenter=mod_send0(mod_window,"rootViewController");CALL(mod_msg,void,(u32,u32,u32,u32,u32))(presenter,mod_sel("presentViewController:animated:completion:"),photo_picker,1,0);
}
static void photo_open(void){
    if(mod_selected<GUEST_MOD_UNIT_COUNT){photo_status("Select a custom card first.");return;}
    if(!photo_panel){u32 parent=mod_send0(mod_send0(mod_window,"rootViewController"),"view");photo_panel=mod_send0(mod_send0(mod_class("UIView"),"alloc"),"init");mod_frame(photo_panel,(ModRect){0,54,320,458});mod_send1(photo_panel,"setBackgroundColor:",mod_color(.06f,.09f,.14f));mod_send1(parent,"addSubview:",photo_panel);
        mod_label(photo_panel,"CUSTOM CARD PHOTOS",(ModRect){12,8,296,32},18);mod_button(photo_panel,"Choose icon photo",32,(ModRect){12,60,296,44});mod_button(photo_panel,"Choose troop photo",33,(ModRect){12,120,296,44});
        mod_label(photo_panel,"Select from your phone's Photos.",(ModRect){12,190,296,28},12);mod_label(photo_panel,"Troop pictures are static images.",(ModRect){12,225,296,28},12);mod_label(photo_panel,"Restart Clash after saving photos.",(ModRect){12,260,296,28},12);mod_button(photo_panel,"Back",34,(ModRect){12,360,296,40});
    }
    mod_send1(photo_panel,"setHidden:",0);photo_status("Choose an icon or troop photo.");
}
