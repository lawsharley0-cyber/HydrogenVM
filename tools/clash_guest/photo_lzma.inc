/* Literal-only LZMA1 encoder. The legacy decoder accepts a known output size;
 * compression runs wholly inside the ARMv7 guest, without host game APIs. */
struct PhotoRange {unsigned long long low;u32 range,cache,count,at,cap;u8 *out;int ok;};
static void photo_put(struct PhotoRange *e,u32 v){if(e->at>=e->cap){e->ok=0;return;}e->out[e->at++]=(u8)v;}
static void photo_shift(struct PhotoRange *e){
    u32 low=(u32)e->low,carry=(u32)(e->low>>32);
    if(low<0xff000000u || carry){u32 temp=e->cache;do{photo_put(e,temp+carry);temp=255;}while(--e->count);e->cache=low>>24;}
    ++e->count;e->low=(unsigned long long)(low<<8);
}
static void photo_bit(struct PhotoRange *e,u32 *prob,u32 bit){
    u32 bound=(e->range>>11)*(*prob);
    if(!bit){e->range=bound;*prob+=(2048-*prob)>>5;}else{e->low+=bound;e->range-=bound;*prob-=*prob>>5;}
    if(e->range<0x1000000u){e->range<<=8;photo_shift(e);}
}
static u32 photo_lzma(const u8 *input,u32 size,u8 *out,u32 cap){
    if(cap<16)return 0;struct PhotoRange e={0,0xffffffffu,0,1,0,cap,out,1};
    photo_put(&e,0x5d);photo_put(&e,0);photo_put(&e,0);photo_put(&e,4);photo_put(&e,0);
    for(u32 j=0;j<4;++j)photo_put(&e,size>>(j*8));
    u32 literal[8][0x300],match[4];
    for(u32 i=0;i<8;++i)for(u32 j=0;j<0x300;++j)literal[i][j]=1024;
    for(u32 i=0;i<4;++i)match[i]=1024;
    u32 previous=0;
    for(u32 i=0;i<size && e.ok;++i){photo_bit(&e,&match[i&3],0);u32 node=1,value=input[i];
        for(int bit=7;bit>=0;--bit){u32 b=(value>>bit)&1;photo_bit(&e,&literal[previous>>5][node],b);node=(node<<1)|b;}previous=value;
    }
    for(u32 i=0;i<5;++i)photo_shift(&e);
    return e.ok?e.at:0;
}
