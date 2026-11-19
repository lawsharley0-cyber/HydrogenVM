/* Inspect duration before a compressed file can allocate decoded PCM. */
static u32 music_le16(const u8 *p){return (u32)p[0]|(u32)p[1]<<8;}
static u32 music_le32(const u8 *p){return music_le16(p)|music_le16(p+2)<<16;}
static int music_read_at(void *file,u32 position,u8 *out,u32 count){
    return !CALL(symbol("fseek"),int,(void*,int,int))(file,(int)position,0) &&
        CALL(symbol("fread"),u32,(void*,u32,u32,void*))(out,1,count,file)==count;
}
static u32 music_duration_bound(const char *path,u32 size){
    void *file=CALL(symbol("fopen"),void*,(const char*,const char*))(path,"rb");if(!file)return 0;
    u8 head[20];u32 seconds=0;
    if(size>=12 && music_read_at(file,0,head,12)){
        if(music_le32(head)==0x46464952u && music_le32(head+8)==0x45564157u){
            u32 offset=12,rate=0;
            while(offset<=size-8){
                if(!music_read_at(file,offset,head,8))break;
                u32 kind=music_le32(head),length=music_le32(head+4);offset+=8;
                if(length>size-offset)break;
                if(kind==0x20746d66u){
                    if(length<16 || !music_read_at(file,offset,head,16))break;
                    u32 format=music_le16(head),channels=music_le16(head+2),samples=music_le32(head+4),bits=music_le16(head+14);
                    if((format!=1 && format!=3) || !channels || channels>2 || !samples || samples>192000 ||
                       (bits!=8 && bits!=16 && bits!=24 && bits!=32) || (format==3 && bits!=32))break;
                    rate=samples*channels*(bits>>3);
                    if(music_le32(head+8)!=rate || music_le16(head+12)!=channels*(bits>>3))break;
                }else if(kind==0x61746164u){if(rate && length && length<=rate*180u)seconds=mod_div(length+rate-1,rate);break;}
                offset+=length;if(length&1){if(offset==size)break;++offset;}
            }
        }else{
            u32 offset=0,sample_rate=0,total_samples=0,frames=0;
            if(head[0]=='I' && head[1]=='D' && head[2]=='3'){
                if((head[6]|head[7]|head[8]|head[9])&128)goto finished;
                offset=10+((u32)head[6]<<21| (u32)head[7]<<14 | (u32)head[8]<<7 | head[9]);
                if(head[5]&16)offset+=10;
            }
            while(offset<size){
                if(size-offset==128 && music_read_at(file,offset,head,3) && head[0]=='T' && head[1]=='A' && head[2]=='G'){offset=size;break;}
                if(size-offset<4 || !music_read_at(file,offset,head,4))break;
                u32 version=(head[1]>>3)&3,layer=(head[1]>>1)&3,index=head[2]>>4,frequency=(head[2]>>2)&3;
                if(head[0]!=255 || (head[1]&224)!=224 || version==1 || layer!=1 || !index || index==15 || frequency==3)break;
                static const u32 rates[3]={44100,48000,32000};
                static const u32 bitrate[2][15]={{0,32,40,48,56,64,80,96,112,128,160,192,224,256,320},{0,8,16,24,32,40,48,56,64,80,96,112,128,144,160}};
                u32 hz=rates[frequency]>>(version==3?0:version==2?1:2);
                if(sample_rate && sample_rate!=hz)break;sample_rate=hz;
                total_samples+=version==3?1152:576;
                if(total_samples>sample_rate*180u)break;
                u32 length=mod_div((version==3?144000u:72000u)*bitrate[version!=3][index],hz)+((head[2]>>1)&1);
                if(length<4 || length>size-offset)break;
                offset+=length;++frames;
            }
            if(frames && offset==size && total_samples<=sample_rate*180u)seconds=mod_div(total_samples+sample_rate-1,sample_rate);
        }
    }
finished:
    if(CALL(symbol("fclose"),int,(void*))(file))seconds=0;
    return seconds;
}
