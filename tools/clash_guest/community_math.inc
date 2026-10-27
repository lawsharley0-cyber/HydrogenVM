/* Offline calendar policy: Monday00:00 UTC. Native client carries weekly
 * scores, but its archived data does not specify the retired server's clock. */
static u32 community_week_start(u32 now) {
    if(now<345600u)return 0; // First nonnegative Unix Monday: January5,1970.
    u32 value=now-345600u,remainder=0;
    for(int bit=31;bit>=0;--bit) {
        remainder=(remainder<<1)|((value>>bit)&1u);
        if(remainder>=604800u)remainder-=604800u;
    }
    return now-remainder;
}
