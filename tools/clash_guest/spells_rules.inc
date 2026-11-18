#define SPELL_FIELDS 5
enum {SPELL_DAMAGE,SPELL_RADIUS,SPELL_DURATION,SPELL_SPEED,SPELL_COST};
struct SpellRecord {u32 id,value[SPELL_FIELDS];};
static u32 spell_default(u32 field){return field==SPELL_COST?0:100;}
static int spell_value_valid(u32 field,u32 value,u32 caps){
    if(field>=SPELL_FIELDS)return 0;
    if(!(caps&(1u<<field)))return value==spell_default(field);
    if(field==SPELL_COST)return value<=10;
    return value>=(field==SPELL_DAMAGE?10u:25u) && value<=(field==SPELL_DAMAGE?800u:400u);
}
