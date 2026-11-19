/* Only a live escrow owns the current wallet. Completed/cancelled/abandoned
 * records retain historical accounting, independent of later native actions. */
static int friendly_wallet_valid(u32 state,int clan,int gold,int free_gold,
                                 int before,int after,int free_before,int free_after) {
    if(state<1 || state>5 || gold<0 || gold>2000000000 || free_gold<0 || free_gold>gold ||
       before<5 || before>2000000000 || after!=before-5 || free_before<0 || free_before>before ||
       free_after!=(free_before<after?free_before:after))return 0;
    if(state==1 || state==2)return clan==2 && gold==after && free_gold==free_after;
    // Pending leave/settings retain membership until their native command is
    // applied. Historical bouts must not invalidate those durable journals.
    return clan==2 || clan==3 || clan==4;
}
