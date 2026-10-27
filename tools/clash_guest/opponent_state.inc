#include "opponent_names.inc"
#include "opponent_math.inc"
/* GBO1: non-repeating paid-match choice, atomic with its original entry debit.
 * It stores no rewards/wallet expectations and never rewrites earned progress. */
static struct OpponentHistory opponent_history;
static int opponent_enabled;
static void write_opponent(void *file,u32 body_hash) {
    if(!opponent_history.serial)return;
    u32 h[10]={0x314f4247u,1,opponent_history.rng,opponent_history.serial,opponent_history.variant,
        opponent_history.name,opponent_history.style,opponent_history.seed,body_hash,0};
    h[9]=checksum((const u8*)h,36);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h)
        fail("GuestOffline: opponent choice checkpoint write failed");
}
static void read_opponent(void *file,u32 body_hash) {
    u32 h[10]={0x314f4247u};
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,36,file)!=36 || h[1]!=1 || !h[2] ||
       !h[3] || h[3]>1000000000u || h[4]>=GUEST_ARENA_PROFILE_COUNT || h[5]>=GUEST_OPPONENT_NAME_COUNT ||
       h[6]>=3 || !h[7] || h[7]>0x7fffffffu || h[8]!=body_hash || h[9]!=checksum((const u8*)h,36))
        fail("GuestOffline: invalid opponent choice checkpoint; original retained");
    opponent_history=(struct OpponentHistory){h[2],h[3],h[4],h[5],h[6],h[7]};
}
