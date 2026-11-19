#define MECH_FIELDS 9
enum {MECH_COUNT,MECH_SPAWN_COUNT,MECH_SPAWN_TYPE,MECH_SHOT,MECH_IMPACT_COUNT,MECH_IMPACT_TYPE,MECH_PAUSE,MECH_DELAY,MECH_ELIXIR};
struct MechRecord {u32 id,value[MECH_FIELDS];};
static int mech_value_valid(u32 f,u32 v,u32 troops,u32 shots){
    if(f>=MECH_FIELDS)return 0;if(!v)return 1;
    if(f==MECH_COUNT)return v<=16;
    if(f==MECH_SPAWN_COUNT || f==MECH_IMPACT_COUNT)return v<=8;
    if(f==MECH_SPAWN_TYPE || f==MECH_IMPACT_TYPE)return v<=troops;
    if(f==MECH_SHOT)return v<=shots;
    if(f==MECH_PAUSE)return v>=500 && v<=60000;
    if(f==MECH_DELAY)return v>=100 && v<=10000;
    return v<=10;
}
