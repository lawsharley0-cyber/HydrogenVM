/* Migrate only the opaque, canonically encoded StreamEntry identity namespace.
 * The full entry/header/native-body validation precedes this operation. */
static int friendly_identity_migration(u8 *entry,u32 entry_size,u8 *result,u32 result_size,u32 *namespace_value) {
    if(*namespace_value==2)return 0;
    if(*namespace_value || entry_size<3 || entry[0]!=10 || entry[1]!=0 ||
       (result_size && (result_size<3 || result[0]!=11 || result[1]!=0)))
        fail("GuestOffline: noncanonical legacy friendly identity; original retained");
    entry[1]=2;if(result_size)result[1]=2;
    *namespace_value=2;return 1;
}
