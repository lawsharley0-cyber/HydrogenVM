/* FGR1: immutable append-only records. FGC1 links a required prefix to CRG2.
 * FNV is an integrity checksum, not authentication. No guest/native pointers. */
#define FORGE_MAX_CARDS 16u
struct ForgeRecord {u32 base,bytes;char name[100];};
static int forge_space(u32 c) {
    return c==32 || c==160 || c==0x1680 || (c>=0x2000 && c<=0x200a) || c==0x202f || c==0x205f || c==0x3000;
}
static int forge_name_valid(const char *name,u32 bytes) {
    if(!bytes || bytes>96)return 0;
    u32 at=0,count=0,visible=0;
    while(at<bytes) {
        u32 c=(u8)name[at++],need=0,min=0;
        if(c>=0xc2 && c<=0xdf){c&=31;need=1;min=0x80;}
        else if(c>=0xe0 && c<=0xef){c&=15;need=2;min=0x800;}
        else if(c>=0xf0 && c<=0xf4){c&=7;need=3;min=0x10000;}
        else if(c>=0x80)return 0;
        if(need>bytes-at)return 0;
        for(u32 i=0;i<need;++i){u32 b=(u8)name[at++];if((b&0xc0)!=0x80)return 0;c=(c<<6)|(b&63);}
        if(c<min || c>0x10ffff || (c>=0xd800 && c<=0xdfff) || c<32 || (c>=127 && c<=159) || c==0x2028 || c==0x2029)return 0;
        if(!forge_space(c))visible=1;
        if(++count>32)return 0;
    }
    return visible;
}
static int forge_record_valid(const struct ForgeRecord *r) {
    if(!forge_name_valid(r->name,r->bytes))return 0;
    for(u32 i=r->bytes;i<100;++i)if(r->name[i])return 0;
    return 1; // The pinned blueprint catalog separately validates base IDs.
}
static int forge_header_valid(const u32 h[5]) {
    return h[0]==0x31524746u && h[1]==1 && h[2]>0 && h[2]<=FORGE_MAX_CARDS && h[4]==checksum((const u8*)h,16);
}
static int forge_link_valid(const u32 h[5],u32 save_hash,const struct ForgeRecord *records,u32 loaded) {
    return h[0]==0x31434746u && h[1]>0 && h[1]<=loaded && h[1]<=FORGE_MAX_CARDS && h[3]==save_hash &&
        h[4]==checksum((const u8*)h,16) && h[2]==checksum((const u8*)records,h[1]*sizeof *records);
}
static int forge_equal(const char *a,const char *b){while(*a && *a==*b){++a;++b;}return *a==*b;}
static u32 forge_length(const char *s){u32 n=0;while(s[n])++n;return n;}
static u32 forge_key_slot(const char *key) {
    const char *prefix="TID_MODLAB_";u32 at=0,slot=0;if(!key)return FORGE_MAX_CARDS;
    while(prefix[at]){if(key[at]!=prefix[at])return FORGE_MAX_CARDS;++at;}
    for(u32 i=0;i<4;++i){u32 c=(u8)key[at++];if(c<'0'||c>'9')return FORGE_MAX_CARDS;slot=slot*10+c-'0';}
    return !key[at] && slot<FORGE_MAX_CARDS?slot:FORGE_MAX_CARDS;
}
static void forge_identifier(char out[24],const char *prefix,u32 slot) {
    u32 at=0;while(prefix[at]){out[at]=prefix[at];++at;}
    out[at++]='0';out[at++]='0';out[at++]=slot>=10?'1':'0';
    out[at++]=(char)('0'+(slot>=10?slot-10:slot));out[at]=0;
}
static int forge_emit(char *out,u32 cap,u32 *at,const char *text,int quote) {
    if(quote){if(*at>=cap)return 0;out[(*at)++]='"';}
    while(*text){if(*at>=cap)return 0;out[(*at)++]=*text;if(quote && *text=='"'){if(*at>=cap)return 0;out[(*at)++]='"';}++text;}
    if(quote){if(*at>=cap)return 0;out[(*at)++]='"';}return 1;
}
static int forge_expand(char *out,u32 cap,const char *text,const struct ForgeRecord *record,u32 slot,u32 *size) {
    if(slot>=FORGE_MAX_CARDS || !forge_record_valid(record))return 0;
    char ids[6][24];const char *keys[7]={"CARD","UNIT","SHOT","TID","ART","ICON","NAME"};
    forge_identifier(ids[0],"ModLabCard_",slot);forge_identifier(ids[1],"ModLabUnit_",slot);
    forge_identifier(ids[2],"ModLabShot_",slot);forge_identifier(ids[3],"TID_MODLAB_",slot);
    forge_identifier(ids[4],"ModLabArt_",slot);forge_identifier(ids[5],"ModLabIcon_",slot);
    u32 at=0;
    while(*text) {
        if(*text!='@'){if(at>=cap)return 0;out[at++]=*text++;continue;}
        ++text;u32 n=0;char key[8];while(*text && *text!='@' && n<7)key[n++]=*text++;key[n]=0;
        if(*text++!='@')return 0;
        u32 k=0;while(k<7 && !forge_equal(key,keys[k]))++k;
        if(k==7 || !forge_emit(out,cap,&at,k==6?record->name:ids[k],k==6))return 0;
    }
    *size=at;return 1;
}
