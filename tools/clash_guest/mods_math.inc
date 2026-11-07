#define MOD_FIELDS 8
enum {MOD_MOVE,MOD_ATTACK,MOD_DEPLOY,MOD_SIZE,MOD_SHOT_SPEED,MOD_HEALTH,MOD_DAMAGE,MOD_RANGE};
static u32 mod_div(u32 n,u32 d) {
    if(!d)fail("Mod Lab: zero divisor");
    u32 q=0,r=0;for(int i=31;i>=0;--i){u32 carry=r>>31;r=(r<<1)|((n>>i)&1u);if(carry || r>=d){r-=d;q|=1u<<i;}}return q;
}
static int mod_percent_valid(u32 field,u32 value) {
    return field<MOD_FIELDS && value>=(field==MOD_SIZE?25u:10u) && value<=(field==MOD_SIZE?400u:800u);
}
static u32 mod_scaled(u32 base,u32 percent,int inverse) {
    if(base>100000000u || percent<10 || percent>800)fail("Mod Lab: invalid native scale");
    // Divide before multiplication where needed; reject rather than overflow.
    u32 a=inverse?100:percent,b=inverse?percent:100;
    if(base && a>0xffffffffu/base)fail("Mod Lab: scaled value overflow");
    u32 n=mod_div(base*a,b);return base && !n?1:n;
}
static int mod_parse_number(const char *s,u32 *out) {
    if(!s || !*s || !out)return 0;u32 n=0;
    for(u32 i=0;s[i];++i){if(i>=9 || s[i]<'0' || s[i]>'9')return 0;n=n*10u+(u32)(s[i]-'0');}
    *out=n;return 1;
}
