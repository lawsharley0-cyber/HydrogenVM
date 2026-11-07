static int reward_source_valid(u32 kind,u32 id) {
    return kind>=1 && kind<=4 && id<=1000000u && (kind==1 || id==0);
}
static u32 reward_home_offset(int kind) {
    if(kind==2)return 0x78; // Free chest.
    if(kind==3)return 0x90; // Crown chest.
    if(kind==4)return 0x7c; // Purchased chest, original ClaimReward source 4.
    return 0;
}
