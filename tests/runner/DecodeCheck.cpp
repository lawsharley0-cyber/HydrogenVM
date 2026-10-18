// Decoder coverage check: reads "<a|t> <hex32>" lines and prints those the
// decoder turns into an Undefined exit. (For Thumb, hex32 = hw1 << 16 | hw2.)
// Used by tools/decode_fuzz.py together with llvm-mc's disassembler.
#include "core/cpu/Decoder.hpp"
#include <cstdio>
#include <cstring>

using namespace Hydrogen::CPU;

int main() {
    char mode;
    unsigned v;
    while (std::scanf(" %c %x", &mode, &v) == 2) {
        Block b;
        BlockBuilder bb(b, 0x10000, mode == 't');
        bb.beginInsn(0x10000);
        if (mode == 'a') decodeArm(bb, v);
        else { uint32_t len; decodeThumb(bb, uint16_t(v >> 16), uint16_t(v), 14, false, len); }
        bool undef = false;
        for (const Op& o : b.ops) undef |= o.fn == &H_Undefined;
        if (undef) std::printf("%c %08x\n", mode, v);
    }
    return 0;
}
