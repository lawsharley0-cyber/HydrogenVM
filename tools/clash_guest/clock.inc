/* Stamp belongs to the same atomic checkpoint as the native home and journal.
 * Bind only the newly delivered home; an old UI must never undo a paid match.
 */
#include "clock_math.h"
static u32 saved_epoch,delivered_epoch,home_clock_epoch,s_wall_clock;
static int home_clock_tick;
static void *delivered_home,*home_clock_logic;
static u32 wall_epoch(void) {
    u32 timeval[2];
    if(CALL(s_wall_clock,int,(void*,void*))(timeval,0)!=0 || !timeval[0] || timeval[1]>=1000000)
        fail("GuestOffline: checkpoint wall clock unavailable");
    return timeval[0]; // pinned Darwin ARMv7 timeval: two 32-bit words
}
void guest_home_loaded(void *logic) {
    void *home=(void*)FIELD(logic,0x30);
    if(!save_initialized || !delivered_home || home!=delivered_home)return;
    if(!FIELD(logic,0x34) || FIELD(home,4)!=(u32)logic)
        fail("GuestOffline: delivered home clock association mismatch");
    // loadHomeState has associated the decoded objects and performed its own
    // fastForwardTime. Its logic-change scope is still open, before
    // loadingFinished/first tick and pending crown transfer (0xd6d40).
    home_clock_logic=logic;home_clock_tick=(int)FIELD(logic,0x38);
    home_clock_epoch=delivered_epoch;
    if(!home_clock_epoch || home_clock_tick<0)fail("GuestOffline: missing delivered home clock");
    if(!pending_size) {
        u32 now=wall_epoch(),elapsed=guest_elapsed_home_seconds(home_clock_epoch,now);
        if(elapsed) {
            CALL(0x125d7d,void,(void*,int,int))(home,(int)elapsed,-1);
            home_clock_epoch=now;
            CALL(API_printf,int,(const char*,u32))("GuestOffline: measured home elapsed at load=%u seconds\n",elapsed);
        }
    }
}
static void account_home_clock(void *logic,void *home,int advance) {
    int tick=(int)FIELD(logic,0x38);
    u32 accounted=guest_accounted_home_epoch(home_clock_epoch,home_clock_tick,tick);
    if(advance) {
        u32 now=wall_epoch(),elapsed=guest_elapsed_home_seconds(accounted,now);
        if(elapsed) {
            CALL(0x125d7d,void,(void*,int,int))(home,(int)elapsed,-1);
            // Native active ticks are already represented; credit only the
            // remaining loading/claim/closed time, once with its checkpoint.
            home_clock_epoch=now-guest_seconds_from_ticks((u32)tick-(u32)home_clock_tick);
            accounted=now;
            CALL(API_printf,int,(const char*,u32))("GuestOffline: measured paused home elapsed=%u seconds\n",elapsed);
        }
    }
    saved_epoch=accounted;
}
