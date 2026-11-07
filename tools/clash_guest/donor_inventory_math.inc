/* Authored conservation rules, independent of native pointers and file I/O.
 * Each original starter material begins at one. Only committed native
 * transfers may change sent/received; receiving does not erase sent history. */
struct DonorInventoryRow {u32 card,sent,received;};
static const u32 donor_starter_cards[6]={26000000u,26000001u,26000003u,26000013u,28000000u,28000001u};
static int donor_inventory_stock(const struct DonorInventoryRow *row,u32 *stock) {
    if(row->sent>1000000000u || row->received>1000000000u || row->sent>1u+row->received)return 0;
    *stock=1u+row->received-row->sent;
    return *stock<=1000000000u;
}
static int donor_inventory_valid(const struct DonorInventoryRow *rows,u32 *total_sent) {
    u32 sum=0;
    for(u32 i=0;i<6;++i) {
        u32 stock;
        if(rows[i].card!=donor_starter_cards[i] || !donor_inventory_stock(rows+i,&stock) ||
           rows[i].sent>1000000000u-sum)return 0;
        sum+=rows[i].sent;
    }
    *total_sent=sum;return 1;
}
