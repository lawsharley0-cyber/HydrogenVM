struct OpponentHistory {u32 rng,serial,variant,name,style,seed;};
static u32 opponent_random(u32 *state) {
    u32 n=*state;if(!n)n=0x9e3779b9u;
    n^=n<<13;n^=n>>17;n^=n<<5;*state=n;return n;
}
static u32 opponent_remainder(u32 value,u32 divisor) {
    u32 n=0;for(int bit=31;bit>=0;--bit) {n=(n<<1)|((value>>bit)&1u);if(n>=divisor)n-=divisor;}return n;
}
static int opponent_choose(struct OpponentHistory *h,const int *scores,const unsigned *decks,u32 count,int score) {
    if(!count || count>64 || score<0 || score>100000 || h->serial>=1000000000u ||
       (h->serial && (h->variant>=count || h->name>=64 || h->style>=3)))return 0;
    int nearest=100001;u32 eligible[64],size=0;
    for(u32 i=0;i<count;++i) {
        if(scores[i]<0 || scores[i]>100000)return 0;
        int delta=score>scores[i]?score-scores[i]:scores[i]-score;
        if(delta<nearest)nearest=delta;
    }
    for(u32 i=0;i<count;++i) {
        int delta=score>scores[i]?score-scores[i]:scores[i]-score;
        if(delta<=nearest+100 && (!h->serial || decks[i]!=decks[h->variant]))eligible[size++]=i;
    }
    if(!size)return 0;
    u32 variant=eligible[opponent_remainder(opponent_random(&h->rng),size)];
    u32 name=opponent_remainder(opponent_random(&h->rng),h->serial?63:64);
    if(h->serial && name>=h->name)++name;
    u32 style=opponent_remainder(opponent_random(&h->rng),h->serial?2:3);
    if(h->serial && style>=h->style)++style;
    h->variant=variant;h->name=name;h->style=style;
    h->seed=opponent_random(&h->rng)&0x7fffffffu;if(!h->seed)h->seed=1;
    ++h->serial;return 1;
}
