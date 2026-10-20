#pragma once
#include "Graphics.hpp"

namespace Hydrogen::Graphics {
// Owns the provider bytes so fonts survive release of the provider/CFData.
class TrueTypeFont {
public:
    static std::shared_ptr<TrueTypeFont> load(std::vector<uint8_t> bytes);
    ~TrueTypeFont();
    std::string name(bool family = false) const;
    int glyph(uint32_t character) const;
    int glyphCount() const;
    uint32_t traits() const;
    float underlineThickness(float size) const;
    float ascent(float size) const;
    float descent(float size) const;
    float leading(float size) const;
    float advance(int glyph, float size) const;
    Rect bounds(int glyph, float size) const;
    Path outline(int glyph, float size) const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    TrueTypeFont();
};
}
