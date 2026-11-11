#ifndef FORGE_UNIT_SLOT
#ifdef GUEST_FORGE_SPELLS
#define FORGE_UNIT_SLOT(index) forge_unit_slots[(index)]
#else
#define FORGE_UNIT_SLOT(index) (index)
#define forge_unit_count forge_loaded_count
#endif
#endif
