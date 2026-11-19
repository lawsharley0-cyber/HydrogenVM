/* Host-only private-fixture verifier. Never linked into the game. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "generated.h"
typedef unsigned int u32;
typedef unsigned char u8;
static u8 saved_home[65536];
static u32 saved_size,avatar_offset;
#undef API_printf
#define API_printf 0
#define CALL(address,result,args) printf
static void fail(const char *why) {fprintf(stderr,"%s\n",why);exit(2);}
#include "reader.inc"
#include "sector.inc"
int main(int argc,char **argv) {
    if(argc!=5 && argc!=7)return 2;
    FILE *file=fopen(argv[1],"rb");if(!file)return 2;
    u32 header[4];
    if(fread(header,1,sizeof header,file)!=sizeof header || header[0]!=0x31475243 || header[1]>sizeof saved_home)return 2;
    saved_size=header[1];avatar_offset=header[2];
    if(avatar_offset>=saved_size || fread(saved_home,1,saved_size,file)!=saved_size)return 2;
    fclose(file);
    u8 actual[65536],expected[65536];
    int mission=atoi(argv[2]);
    if(mission<0 || mission>=(argc==7?GUEST_ARENA_PROFILE_COUNT:7))return 2;
    u32 size=argc==7?assemble_sector_custom(actual,sizeof actual,&arena1_sectors[mission],argv[5],(u32)strtoul(argv[6],0,10)):
                    assemble_sector(actual,sizeof actual,mission);
    // Private diagnostic captures are useful even when the independent oracle fails.
    if(strcmp(argv[4],"-")) {file=fopen(argv[4],"wb");if(!file)return 2;fwrite(actual,1,size,file);fclose(file);}
    file=fopen(argv[3],"rb");if(!file)return 2;
    size_t wanted=fread(expected,1,sizeof expected,file);int extra=fgetc(file);fclose(file);
    if(extra!=EOF || wanted!=size || memcmp(expected,actual,size)) {
        size_t at=0;while(at<wanted && at<size && expected[at]==actual[at])++at;
        fprintf(stderr,"Sector differs at %zu (sizes %u / %zu; actual %u, expected %u)\n",at,size,wanted,at<size?actual[at]:0,at<wanted?expected[at]:0);return 1;
    }
    return 0;
}
