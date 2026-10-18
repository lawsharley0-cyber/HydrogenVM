// ArgonVM — decoder entry points.
#pragma once
#include "BlockBuilder.hpp"

namespace Argon::CPU {

enum class DecodeResult { Continue, End, EndNeedsFallthrough };

// Decode one A32 instruction (cond handled inside).
DecodeResult decodeArm(BlockBuilder& bb, uint32_t insn);

// Decode one T32 instruction. `hw1` is the first halfword, `hw2` the second
// (ignored for 16-bit encodings). `itCond` is the IT condition for this slot,
// or 14 (AL) outside an IT block. Sets `len` to 2 or 4.
DecodeResult decodeThumb(BlockBuilder& bb, uint16_t hw1, uint16_t hw2, uint32_t itCond, bool inIt, uint32_t& len);

// VFP / Advanced SIMD (Phase 2). Until implemented these emit Undefined.
DecodeResult decodeArmVfp(BlockBuilder& bb, uint32_t insn);
DecodeResult decodeArmNeon(BlockBuilder& bb, uint32_t insn);
DecodeResult decodeThumbVfpNeon(BlockBuilder& bb, uint32_t insn32);

// Is this a 32-bit Thumb encoding (first halfword)?
inline bool thumbIs32(uint16_t hw1) { return (hw1 >> 11) >= 0x1D; }

} // namespace Argon::CPU
