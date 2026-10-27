/* Offline shop calendar: UTC midnight, Sunday=1..Saturday=7. The retired
 * service's reset zone is not specified by the archived client. */
static u32 shop_divide(u32 value,u32 divisor) {
    u32 quotient=0,remainder=0;
    for(int bit=31;bit>=0;--bit) {
        remainder=(remainder<<1)|((value>>bit)&1u);
        if(remainder>=divisor) {remainder-=divisor;quotient|=1u<<bit;}
    }
    return quotient;
}
static int shop_eligible(int wins,int arena) {return wins>=7 && arena>54000000 && arena<=54000020;}
static u32 shop_day(u32 now) {return shop_divide(now,86400u);}
static u32 shop_weekday(u32 day) {u32 n=day+4u;return n-shop_divide(n,7u)*7u+1u;}
static u32 shop_seconds_left(u32 now) {return 86400u-(now-shop_day(now)*86400u);}
// Store the calendar generation in the existing native seed field. No
// independent wallet/item ledger or rewards are introduced.
#define GUEST_SHOP_SEED_BASE 1000000000u
static int shop_should_refresh(u32 seed,u32 now) {
    if(seed==0 || seed==0xffffffffu)return 1;
    if(seed<GUEST_SHOP_SEED_BASE || seed>GUEST_SHOP_SEED_BASE+49710u)return -1;
    return seed<GUEST_SHOP_SEED_BASE+shop_day(now);
}
static int shop_fields_valid(u32 seed,int weekday,u32 count) {
    if(seed==0 || seed==0xffffffffu)return count==0 && (weekday==0 || weekday==-1);
    if(seed<GUEST_SHOP_SEED_BASE || seed>GUEST_SHOP_SEED_BASE+49710u)return 0;
    u32 day=shop_weekday(seed-GUEST_SHOP_SEED_BASE);
    return weekday==(int)day && count==(day==1?6u:3u);
}
