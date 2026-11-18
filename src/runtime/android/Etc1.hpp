// HydrogenVM — ETC1 (GL_ETC1_RGB8_OES) texture decoding.
//
// ETC1 is the compressed texture format every Android GPU of the OpenGL ES 2 era supports
// and games ship for it. Apple's GPUs and desktop drivers don't take it, so an Android game's
// ETC1 uploads are decoded here to RGB and uploaded uncompressed.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Hydrogen::Runtime::Android {

constexpr uint32_t kGlEtc1Rgb8 = 0x8D64;

// Bytes of ETC1 data for an image of `w` x `h` (4x4 blocks of 8 bytes).
inline size_t etc1Size(int w, int h) { return size_t((w + 3) / 4) * size_t((h + 3) / 4) * 8; }

// Decodes `w` x `h` pixels from `data` (at least etc1Size bytes) to tightly packed RGB8,
// top row first as in the source.
void decodeEtc1Block(const uint8_t* data, uint8_t (&rgb)[4][4][3]);
std::vector<uint8_t> decodeEtc1(const uint8_t* data, int w, int h);

} // namespace Hydrogen::Runtime::Android
