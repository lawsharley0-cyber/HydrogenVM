/* GNI1 binds inventory provenance to BOTH native actors in their atomic file.
 * FNV detects accidental corruption, not hostile modification/authentication. */
#include "donor_inventory_math.inc"
static struct DonorInventoryRow donor_inventory[6];
static u32 donor_inventory_count,donor_inventory_body_hash,donor_inventory_loaded;
static void write_donor_inventory(void *file,u32 body_hash) {
    if(!donor_inventory_count)return;
    u32 sent;
    if(donor_inventory_count!=6 || !donor_size || !donor_inventory_valid(donor_inventory,&sent))
        fail("GuestOffline: invalid donor inventory before checkpoint");
    u32 h[7]={0x31494e47u,1,6,body_hash,checksum(donor_home,donor_size),
        checksum((const u8*)donor_inventory,sizeof donor_inventory),0};
    h[6]=checksum((const u8*)h,24);
    if(CALL(s_write,u32,(const void*,u32,u32,void*))(h,1,sizeof h,file)!=sizeof h ||
       CALL(s_write,u32,(const void*,u32,u32,void*))(donor_inventory,1,sizeof donor_inventory,file)!=sizeof donor_inventory)
        fail("GuestOffline: donor inventory checkpoint write failed");
}
static void read_donor_inventory(void *file,u32 body_hash) {
    u32 h[7]={0x31494e47u},sent;
    if(CALL(s_read,u32,(void*,u32,u32,void*))(h+1,1,24,file)!=24 ||
       h[1]!=1 || h[2]!=6 || h[3]!=body_hash || h[6]!=checksum((const u8*)h,24) ||
       CALL(s_read,u32,(void*,u32,u32,void*))(donor_inventory,1,sizeof donor_inventory,file)!=sizeof donor_inventory ||
       h[5]!=checksum((const u8*)donor_inventory,sizeof donor_inventory) ||
       !donor_inventory_valid(donor_inventory,&sent))
        fail("GuestOffline: invalid donor inventory checkpoint; original retained");
    donor_inventory_count=6;donor_inventory_body_hash=h[4];donor_inventory_loaded=1;
}
