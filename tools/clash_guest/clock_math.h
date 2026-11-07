/* Native timers use 20 ticks/second. All bundled cooldowns fit in 30 days;
 * cap the original seconds-to-ticks path and credit no unstamped/negative time.
 */
static u32 guest_seconds_from_ticks(u32 ticks) {
    // Exact unsigned division by 20 over 32-bit input. ARMv7 has UMULL but
    // this freestanding payload has no compiler-runtime division library.
    return (u32)(((unsigned long long)ticks*0xcccccccdull)>>36);
}
static u32 guest_elapsed_home_seconds(u32 saved,u32 now) {
    if(!saved || now<=saved)return 0;
    u32 elapsed=now-saved,cap=30u*24u*60u*60u;
    return elapsed>cap?cap:elapsed;
}
static u32 guest_accounted_home_epoch(u32 epoch,int first_tick,int tick) {
    if(!epoch || first_tick<0 || tick<first_tick)
        fail("GuestOffline: native home clock moved backwards");
    u32 seconds=guest_seconds_from_ticks((u32)tick-(u32)first_tick);
    if(epoch>~0u-seconds)fail("GuestOffline: native home clock overflow");
    return epoch+seconds;
}
