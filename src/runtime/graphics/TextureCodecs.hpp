// CPU decoders for Android compressed GLES textures.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace Hydrogen::Graphics {

struct DecodedTexture {
    // Color formats use four RGBA8 components, in source row order.
    // EAC R11/RG11 use one or two normalized float components, preserving signed values.
    int components = 4;
    bool srgb = false;
    std::vector<uint8_t> rgba;
    std::vector<float> redGreen;
};

// Exact GLES byte count, including the minimum PVRTC1 footprint. Empty means
// an unsupported format or invalid dimensions (PVRTC1 requires powers of two).
std::optional<size_t> compressedTextureSize(uint32_t format, int width, int height);

std::optional<DecodedTexture> decodeCompressedTexture(uint32_t format, int width, int height,
                                                      std::span<const uint8_t> data);

} // namespace Hydrogen::Graphics
