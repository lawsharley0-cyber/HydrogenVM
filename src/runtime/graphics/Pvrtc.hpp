// HydrogenVM — PVRTC1 texture decompression (GL_IMG_texture_compression_pvrtc),
// the compressed format of every 32-bit iPhone GPU.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace Hydrogen::Graphics {

// Decodes a PVRTC1 image (4 or 2 bits per pixel) to straight RGBA8, row 0 first
// (same row order as the data). `withAlpha` false forces alpha to 255 (the RGB
// variants). Width and height are powers of two (smaller sizes are padded up
// to the minimum block grid as the format requires). Returns false if `n` is
// too small for the image.
bool decodePvrtc(const uint8_t* data, size_t n, int width, int height, bool twoBpp, bool withAlpha, std::vector<uint8_t>& rgba);

} // namespace Hydrogen::Graphics
