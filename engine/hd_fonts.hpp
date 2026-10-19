// The full-colour layer's fonts (hd.hpp): glyph tables made by tools/hd_fonts.py.
// Portable data; the board and the desktop runtime compile the same tables.
#pragma once

#include <cstdint>

namespace carts::hd_fonts {

struct Glyph {
  uint32_t cp;        // the character
  uint32_t off;       // its coverage in the font's bits (4 bits a pixel, rows padded to a byte)
  uint8_t w, h;       // its box
  int8_t x, y;        // the box's top left from the pen (y from the baseline, down positive)
  uint8_t adv;        // how far the pen moves
};

struct Font {
  const char* name;   // "mono", "bold", "vt"
  uint8_t size;       // px
  uint8_t ascent, descent;
  uint16_t count;
  const Glyph* glyphs;  // by cp, ascending
  const uint8_t* bits;
};

extern const Font kFonts[];
extern const int kFontCount;

}  // namespace carts::hd_fonts
