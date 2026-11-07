/* The local clan always has one leader. A leadership handoff makes the human
 * co-leader and the persisted NPC leader; a retired NPC cannot lead/donate. */
static int clan_roles_valid(int human,int present,int npc) {
    if(present!=0 && present!=1)return 0;
    if(human==2)return !present || npc==1 || npc==3 || npc==4;
    return human==4 && present && npc==2;
}
