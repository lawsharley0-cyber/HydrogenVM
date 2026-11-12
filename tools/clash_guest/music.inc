/* Persistent menu soundtrack. Original music and training assets stay bundled. */
#include "mods_math.inc"
#include "music_limits.inc"
enum {MUSIC_MENU,MUSIC_BATTLE,MUSIC_CONTEXTS};
struct MusicTrack {u32 generation,bytes,hash,seconds;};
static struct MusicTrack music_tracks[MUSIC_CONTEXTS];
static u32 music_ready,music_selected;
#define music_generation music_tracks[music_selected].generation
#define music_bytes music_tracks[music_selected].bytes
#define music_hash music_tracks[music_selected].hash
static const char *music_headers[MUSIC_CONTEXTS]={"/menu.bin","/battle.bin"};
static u32 music_mask(void){
    return (music_tracks[MUSIC_MENU].generation?1u:0u)|(music_tracks[MUSIC_BATTLE].generation?2u:0u);
}
static char music_folder[512],music_file[560];
static void music_path(char out[560],u32 generation){
    const char *hex="0123456789abcdef";char name[32]="/track-";
    for(u32 i=0;i<8;++i)name[7+i]=hex[(generation>>(28-4*i))&15];
    const char *ext=".audio";u32 i=0;while(ext[i]){name[15+i]=ext[i];++i;}name[15+i]=0;
    path_append(out,music_folder,name,560);
}
static int music_check(const char *path,u32 bytes,u32 expected){
    void *f=CALL(symbol("fopen"),void*,(const char*,const char*))(path,"rb");if(!f)return 0;
    u8 data[4096];u32 left=bytes,hash=2166136261u;int ok=bytes>0 && bytes<=8388608u;
    while(ok && left){u32 n=left<sizeof data?left:sizeof data;
        ok=CALL(symbol("fread"),u32,(void*,u32,u32,void*))(data,1,n,f)==n;
        for(u32 i=0;ok && i<n;++i)hash=(hash^data[i])*16777619u;left-=n;}
    u8 extra;ok=ok && !CALL(symbol("fread"),u32,(void*,u32,u32,void*))(&extra,1,1,f) && !CALL(symbol("ferror"),int,(void*))(f);
    int closed=CALL(symbol("fclose"),int,(void*))(f);return ok && !closed && hash==expected;
}
static int music_loop_table(void){
    u32 seconds=music_tracks[MUSIC_MENU].seconds;
    const char *music_loop_csv=music_csv_variants[music_mask()];
    const char *token="@MUSIC_SECONDS@";char output[MUSIC_CSV_CAPACITY],number[4];
    u32 tens=mod_div(seconds,10),hundreds=mod_div(seconds,100),digits=0;
    if(hundreds)number[digits++]=(char)('0'+hundreds);
    if(tens)number[digits++]=(char)('0'+tens-hundreds*10);
    number[digits++]=(char)('0'+seconds-tens*10);
    u32 bytes=0;
    for(u32 i=0;music_loop_csv[i];){
        if(music_loop_csv[i]=='@'){
            u32 n=0;while(token[n] && token[n]==music_loop_csv[i+n])++n;
            if(token[n])return 0;i+=n;
            for(u32 j=0;j<digits;++j){if(bytes>=sizeof output)return 0;output[bytes++]=number[j];}
        }else{if(bytes>=sizeof output)return 0;output[bytes++]=music_loop_csv[i++];}
    }
    char path[560],temporary[570];
    path_append(path,music_folder,"/menu.csv",sizeof path);path_append(temporary,path,".tmp",sizeof temporary);
    void *file=CALL(symbol("fopen"),void*,(const char*,const char*))(temporary,"wb");if(!file)return 0;
    int ok=CALL(symbol("fwrite"),u32,(const void*,u32,u32,void*))(output,1,bytes,file)==bytes;
    int flushed=CALL(symbol("fflush"),int,(void*))(file),fd=CALL(symbol("fileno"),int,(void*))(file);
    ok=ok && !flushed && fd>=0 && !CALL(symbol("fsync"),int,(int))(fd);
    int closed=CALL(symbol("fclose"),int,(void*))(file);
    return ok && !closed && !CALL(symbol("rename"),int,(const char*,const char*))(temporary,path);
}
static void music_load_track(u32 context){
    char path[560],track[560];path_append(path,music_folder,music_headers[context],sizeof path);
    void *f=CALL(symbol("fopen"),void*,(const char*,const char*))(path,"rb");if(!f)return;
    u32 h[5];int ok=CALL(symbol("fread"),u32,(void*,u32,u32,void*))(h,1,sizeof h,f)==sizeof h;
    u8 extra;ok=ok && !CALL(symbol("fread"),u32,(void*,u32,u32,void*))(&extra,1,1,f) && !CALL(symbol("ferror"),int,(void*))(f);
    int closed=CALL(symbol("fclose"),int,(void*))(f);
    ok=ok && !closed && h[0]==0x3143554du && h[4]==checksum((u8*)h,16);
    if(ok && h[1]){
        music_path(track,h[1]);ok=music_check(track,h[2],h[3]);
        u32 seconds=ok?music_duration_bound(track,h[2]):0;ok=ok && seconds;
        if(ok)music_tracks[context]=(struct MusicTrack){h[1],h[2],h[3],seconds};
    }
    if(!ok)CALL(API_printf,int,(const char*,u32))("Music Lab: rejected assignment %u retained; original track used\n",context);
}
static void music_initialize(void){
    if(music_ready)return;music_ready=1;
    path_append(music_folder,CALL(symbol("getenv"),const char*,(const char*))("HOME"),"/Documents/ClashMusic-v1",sizeof music_folder);
    for(u32 i=0;i<MUSIC_CONTEXTS;++i)music_load_track(i);
    if(music_mask() && !music_loop_table()){
        memset(music_tracks,0,sizeof music_tracks);
        CALL(API_puts,int,(const char*))("Music Lab: cannot publish music table; originals used, assignments retained");
    }
    if(music_generation)music_path(music_file,music_generation);
}
static int music_menu_path(const char *path){
    const char *prefix="res/music/ios/";u32 n=0;while(path[n])++n;
    const char *names[]={"menu_03.m4a","wizmen04.m4a","new_menu_01.m4a"};
    for(u32 k=0;k<3;++k){u32 size=0;while(names[k][size])++size;if(n<14+size)continue;
        u32 start=n-size-14,i=0;
        if(start && path[start-1]!='/')continue;
        while(i<14 && path[start+i]==prefix[i])++i;if(i!=14)continue;
        i=0;while(i<size && path[n-size+i]==names[k][i])++i;if(i==size)return 1;
    }
    return 0;
}
static int music_battle_alias(const char *path){
    const char *suffix="res/music/ios/modlab_battle_0.m4a";
    u32 n=0,size=0;while(path[n])++n;while(suffix[size])++size;
    if(n<size || (n>size && path[n-size-1]!='/'))return -1;
    u32 at=n-size;
    for(u32 i=0;i<size;++i){
        if(i==size-5){if(path[at+i]<'0' || path[at+i]>'2')return -1;}
        else if(path[at+i]!=suffix[i])return -1;
    }
    return (int)(path[n-5]-'0');
}
static int music_battle_allowed(void){
    return save_initialized && committed_wins>=7 && (!battle_started || arena_battle);
}
static u32 music_url(u32 url){
    music_initialize();if(!url)return url;
    u32 msg=symbol("objc_msgSend"),sel=symbol("sel_registerName"),cls=symbol("objc_getClass");
    u32 path=CALL(msg,u32,(u32,u32))(url,CALL(sel,u32,(const char*))("path"));
    const char *text=path?CALL(msg,const char*,(u32,u32))(path,CALL(sel,u32,(const char*))("UTF8String")):0;
    if(!text)return url;
    int alias=music_battle_alias(text);char target[768];const char *kind;
    if(alias>=0){
        if(music_tracks[MUSIC_BATTLE].generation && music_battle_allowed()){
            music_path(target,music_tracks[MUSIC_BATTLE].generation);kind="custom battle";
        }else{
            const char *names[]={"2min_loop_battle_01.m4a","2min_loop_battle_02.m4a","2min_loop_battle_03.m4a"};
            u32 length=0;while(text[length])++length;
            while(length && text[length-1]!='/')--length;
            if(length>=sizeof target)return url;
            memcpy(target,text,length);target[length]=0;
            char prefix[768];memcpy(prefix,target,length+1);path_append(target,prefix,names[alias],sizeof target);
            kind="original training/friendly";
        }
    }else if(music_menu_path(text) && music_tracks[MUSIC_MENU].generation){
        music_path(target,music_tracks[MUSIC_MENU].generation);kind="custom menu";
    }else return url;
    u32 string=CALL(msg,u32,(u32,u32,const char*))(CALL(cls,u32,(const char*))("NSString"),CALL(sel,u32,(const char*))("stringWithUTF8String:"),target);
    CALL(API_printf,int,(const char*,const char*))("Music Lab: opening %s soundtrack\n",kind);
    return CALL(msg,u32,(u32,u32,u32))(CALL(cls,u32,(const char*))("NSURL"),CALL(sel,u32,(const char*))("fileURLWithPath:"),string);
}
int guest_music_audio_open(u32 url,int permission,u32 hint,u32 *out){
    return CALL(symbol("AudioFileOpenURL"),int,(u32,int,u32,u32*))(music_url(url),permission,hint,out);
}
int guest_music_ext_open(u32 url,u32 *out){
    return CALL(symbol("ExtAudioFileOpenURL"),int,(u32,u32*))(music_url(url),out);
}
static void *music_csv_redirect(const char *path,const char *mode,int *handled){
    const char *suffix="res/csv_client/music.csv";
    u32 size=0,n=0;while(suffix[size])++size;while(path[n])++n;
    if(n<size || (n>size && path[n-size-1]!='/'))return 0;
    for(u32 i=0;i<size;++i)if(path[n-size+i]!=suffix[i])return 0;
    music_initialize();if(!music_mask())return 0;
    char dest[560];path_append(dest,music_folder,"/menu.csv",sizeof dest);*handled=1;
    CALL(API_puts,int,(const char*))("Music Lab: using assigned music contexts");
    return CALL(symbol("fopen"),void*,(const char*,const char*))(dest,mode);
}
