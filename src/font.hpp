#pragma once
#include <cstdint>

// Built-in 5x8 pixel font (original, hand-drawn) covering printable ASCII.
// Each glyph is 8 rows; bit 4 of a row is the leftmost pixel.
namespace font {
constexpr int kGlyphW = 5;
constexpr int kGlyphH = 8;
constexpr int kCellW = 6;  // glyph + 1px spacing
constexpr int kCellH = 9;
// Returns 8 row bitmasks for ASCII 32..126 (others map to '?').
const uint8_t* glyph(char c);
}  // namespace font
