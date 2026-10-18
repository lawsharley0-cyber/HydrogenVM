// HydrogenVM — VFPv3 / Advanced SIMD decoding (Phase 2). Not implemented yet:
// every encoding is reported as Undefined so the dispatcher stops with a
// precise "unimplemented instruction at PC" exit instead of misbehaving.
#include "Decoder.hpp"

namespace Hydrogen::CPU {

DecodeResult decodeArmVfp(BlockBuilder& bb, uint32_t insn) {
    bb.exitSimple(&H_Undefined, insn);
    return DecodeResult::End;
}
DecodeResult decodeArmNeon(BlockBuilder& bb, uint32_t insn) {
    bb.exitSimple(&H_Undefined, insn);
    return DecodeResult::End;
}
DecodeResult decodeThumbVfpNeon(BlockBuilder& bb, uint32_t insn32) {
    bb.exitSimple(&H_Undefined, insn32);
    return DecodeResult::End;
}

} // namespace Hydrogen::CPU
