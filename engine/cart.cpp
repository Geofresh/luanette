// The cart runtime: a sandboxed Lua state, its heap and instruction budget, and the cart API
// (docs/api.md) drawing into a 128x128 picture of palette indices. See cart.hpp.
//
// The drawing calls match tools/check.py pixel for pixel (the same line, circle and font rules),
// so a cart's preview on the host is what the board shows.

#include "cart.hpp"
#include "cartsrc.hpp"
#include "hd.hpp"
#include "sfx.hpp"
#include "stage.hpp"
#include "synth.hpp"
#include "ui_lua.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <utility>
#include <vector>

extern "C" {
#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
// Lua's internals, for reading a drawing call's arguments without lua_tonumberx's way round.
#include "lobject.h"
#include "lstate.h"
}

namespace carts {

// ---- the heap ------------------------------------------------------------------
//
// Lua passes the old size to its allocator, so blocks need no header: small ones come from
// size-class free lists carved from 32 KB blocks the host gives (PSRAM on the board), bigger ones
// straight from the host. Everything goes back when the cart closes. The cap is on what Lua asked
// for, as check.py's lupa counts it.
namespace {

constexpr int kClasses = 7;  // 16 .. 1024 bytes
constexpr size_t kSmallMax = 1024, kBlock = 32 * 1024;
constexpr int kMaxBlocks = 96;

int class_of(size_t n) {
  int c = 0;
  size_t cap = 16;
  while (cap < n) cap <<= 1, ++c;
  return c;
}

}  // namespace

struct Heap {
  Host* host;
  size_t used = 0, peak = 0;
  size_t freed = 0;  // bytes given back, all told (a frame's share says whether Lua collected in it)
  void* free_[kClasses] = {};
  uint8_t *top = nullptr, *end = nullptr;
  void* blocks[kMaxBlocks] = {};
  int nblocks = 0;

  void* small(int c) {
    if (void* p = free_[c]) {
      free_[c] = *static_cast<void**>(p);
      return p;
    }
    const size_t bytes = size_t{16} << c;
    if (!top || top + bytes > end) {
      if (nblocks == kMaxBlocks) return nullptr;
      auto* b = static_cast<uint8_t*>(host->alloc_lua(kBlock));
      if (!b) return nullptr;
      // What was left of the last block goes to the free lists, largest pieces first.
      for (int k = kClasses - 1; top && k >= 0; --k)
        while (top + (size_t{16} << k) <= end) {
          *reinterpret_cast<void**>(top) = free_[k];
          free_[k] = top;
          top += size_t{16} << k;
        }
      blocks[nblocks++] = b;
      top = b;
      end = b + kBlock;
    }
    void* p = top;
    top += bytes;
    return p;
  }
  void give_back(void* p, size_t n) {
    if (n <= kSmallMax) {
      const int c = class_of(n);
      *static_cast<void**>(p) = free_[c];
      free_[c] = p;
    } else {
      host->release_lua(p);
    }
  }
  void* get(size_t n) { return n <= kSmallMax ? small(class_of(n)) : host->alloc_lua(n); }
  void close() {
    for (int i = 0; i < nblocks; ++i) host->release_lua(blocks[i]);
    nblocks = 0;
  }
};

namespace {

void* lua_alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
  auto* h = static_cast<Heap*>(ud);
  if (!ptr) osize = 0;  // then it says what kind of object is being made
  if (nsize == 0) {
    if (ptr) h->give_back(ptr, osize);
    h->used -= osize;
    h->freed += osize;
    return nullptr;
  }
  if (h->used - osize + nsize > kMaxMemory) return nullptr;  // Lua collects, then fails "not enough memory"
  if (ptr && (osize <= kSmallMax) == (nsize <= kSmallMax) && osize <= kSmallMax && class_of(osize) == class_of(nsize)) {
    h->used += nsize - osize;
    return ptr;
  }
  void* p = h->get(nsize);
  if (!p) return nullptr;
  if (ptr) {
    std::memcpy(p, ptr, std::min(osize, nsize));
    h->give_back(ptr, osize);
  }
  h->used += nsize - osize;
  h->peak = std::max(h->peak, h->used);
  return p;
}

// ---- the font: 3x5 glyphs in 4x6 cells (check.py's FONT) ---------------------------

constexpr uint16_t glyph(const char* rows) {  // 15 bits, row-major, bit 14 = top left
  uint16_t g = 0;
  for (int i = 0; i < 15; ++i) g = static_cast<uint16_t>(g << 1 | (rows[i] == '1'));
  return g;
}
struct FontEntry {
  char c;
  uint16_t g;
};
constexpr FontEntry kFont[] = {
    {'0', glyph("111101101101111")}, {'1', glyph("010110010010111")}, {'2', glyph("111001111100111")},
    {'3', glyph("111001111001111")}, {'4', glyph("101101111001001")}, {'5', glyph("111100111001111")},
    {'6', glyph("111100111101111")}, {'7', glyph("111001001010010")}, {'8', glyph("111101111101111")},
    {'9', glyph("111101111001111")}, {'A', glyph("010101111101101")}, {'B', glyph("110101110101110")},
    {'C', glyph("011100100100011")}, {'D', glyph("110101101101110")}, {'E', glyph("111100110100111")},
    {'F', glyph("111100110100100")}, {'G', glyph("011100101101011")}, {'H', glyph("101101111101101")},
    {'I', glyph("111010010010111")}, {'J', glyph("001001001101010")}, {'K', glyph("101101110101101")},
    {'L', glyph("100100100100111")}, {'M', glyph("101111111101101")}, {'N', glyph("110101101101101")},
    {'O', glyph("010101101101010")}, {'P', glyph("110101110100100")}, {'Q', glyph("010101101110011")},
    {'R', glyph("110101110101101")}, {'S', glyph("011100010001110")}, {'T', glyph("111010010010010")},
    {'U', glyph("101101101101011")}, {'V', glyph("101101101010010")}, {'W', glyph("101101111111101")},
    {'X', glyph("101101010101101")}, {'Y', glyph("101101010010010")}, {'Z', glyph("111001010100111")},
    {' ', glyph("000000000000000")}, {'.', glyph("000000000000010")}, {',', glyph("000000000010100")},
    {':', glyph("000010000010000")}, {'!', glyph("010010010000010")}, {'?', glyph("111001010000010")},
    {'-', glyph("000000111000000")}, {'+', glyph("000010111010000")}, {'/', glyph("001001010100100")},
    {'%', glyph("101001010100101")}, {'(', glyph("010100100100010")}, {')', glyph("010001001001010")},
    {'=', glyph("000111000111000")}, {'\'', glyph("010010000000000")}, {'"', glyph("101101000000000")},
    {'>', glyph("100010001010100")}, {'<', glyph("001010100010001")}, {'*', glyph("101010101000000")},
    {'#', glyph("101111101111101")}, {';', glyph("000010000010100")}, {'_', glyph("000000000000111")}, {'[', glyph("110100100100110")},
    {']', glyph("011001001001011")}, {'$', glyph("011110010011110")}, {'&', glyph("010101010101011")},
};
constexpr uint16_t kBox = glyph("111101101101111");  // a character the font lacks

struct FontTable {
  uint16_t g[128];
  constexpr FontTable() : g{} {
    for (auto& v : g) v = kBox;
    for (const auto& e : kFont) g[static_cast<unsigned char>(e.c)] = e.g;
  }
};
constexpr FontTable kGlyphs;


}  // namespace

// ---- the cart's state, as the API functions see it ----------------------------------------

struct Cart::Impl {
  Cart* cart;
  Heap heap;
  std::unique_ptr<Stage> stage;  // made by the cart's first 3D call (docs/api.md, "3D")
  std::unique_ptr<Hd> hd;        // made by the cart's first hd call: its four planes from the host (PSRAM)
  void* hd_mem[4] = {};  // the layer's planes (PSRAM)
  bool full = false;       // "-- canvas: full": the layer covers the glass
  Caps caps;               // the host's, read at start
  // Images unpacked for hd.image, newest use last: at most kImageCache bytes of planes (PSRAM).
  struct CachedImage {
    std::string name;
    HdImage img;
    void* mem = nullptr;
    size_t bytes = 0;
  };
  std::vector<CachedImage> images;
  size_t image_bytes = 0;
  // The cart's own sounds, parsed on first play and kept while their text stays the same.
  struct OwnSound {
    std::string text;
    std::shared_ptr<const Sound> sound;
  };
  OwnSound sounds[kSoundLast + 1];
  int logs = 0;  // log() lines this frame (kLogLines at most)
  static constexpr size_t kImageCache = 1024 * 1024;
  // An image by name, unpacked on first use and kept (least recently used dropped past kImageCache).
  const HdImage* image(std::string_view name) {
    for (size_t i = 0; i < images.size(); ++i) {
      if (images[i].name != name) continue;
      if (i + 1 != images.size()) std::rotate(images.begin() + static_cast<long>(i), images.begin() + static_cast<long>(i) + 1, images.end());
      return &images.back().img;
    }
    std::vector<uint8_t> packed;
    int w = 0, h = 0;
    bool alpha = false;
    if (!host->image(name, packed) || !hd_image_info(packed.data(), packed.size(), &w, &h, &alpha)) return nullptr;
    const size_t n = static_cast<size_t>(w) * h, bytes = n * 3;
    while (!images.empty() && image_bytes + bytes > kImageCache) {
      host->release(images.front().mem);
      image_bytes -= images.front().bytes;
      images.erase(images.begin());
    }
    void* mem = host->alloc(bytes);
    if (!mem) return nullptr;
    CachedImage e;
    e.name = std::string(name);
    e.img.w = w;
    e.img.h = h;
    e.img.c = static_cast<uint16_t*>(mem);
    e.img.a = static_cast<uint8_t*>(mem) + n * 2;
    e.mem = mem;
    e.bytes = bytes;
    if (!hd_image_decode(packed.data(), packed.size(), e.img)) {
      host->release(mem);
      return nullptr;
    }
    image_bytes += bytes;
    images.push_back(std::move(e));
    return &images.back().img;
  }
  void drop_images() {
    for (auto& e : images) host->release(e.mem);
    images.clear();
    image_bytes = 0;
  }
  uint8_t* fb = nullptr;
  int color = 7;
  uint8_t pmap[16];
  int cam_x = 0, cam_y = 0;
  int clip_x0 = 0, clip_y0 = 0, clip_x1 = kW, clip_y1 = kH;  // exclusive
  Input in, was;
  int64_t t0_us = 0, frame_t0 = 0, limit_us = 0;
  int ops = 0, budget = 0, polls = 0;  // Lua instructions this frame and the most it may have (count_hook)
  std::string trip_msg;  // why the frame was stopped (tripped_hook raises it again)
  int count_left = 0;  // instructions to the next count when _update ended: _draw goes on from there
  uint32_t rng = 0x9E3779B9u;
  Host* host = nullptr;
  struct Sprite {
    char hex[64] = {};  // the string it was decoded from
    uint8_t px[64];
  } sprites[256];  // 0-255 (API 6; 0-63 before), decoded once (on the board this is in PSRAM)
  uint16_t clear = 1;  // a bit per color: the ones spr(), sspr() and map() leave out (palt; color 0)
  // The map (API 5): MAP's rows, a character a cell, read once and again when MAP is another table;
  // mset() changes cells here, not in MAP's strings. kNoTile is an empty cell.
  static constexpr int kMapW = 128, kMapH = 64;
  static constexpr uint8_t kNoTile = 0xFF;
  uint8_t map_cells[kMapW * kMapH];
  int map_w = 0, map_h = 0;
  bool map_read = false;

  void reset_draw() {
    color = 7;
    for (int i = 0; i < 16; ++i) pmap[i] = static_cast<uint8_t>(i);
    clear = 1;
    cam_x = cam_y = 0;
    clip_x0 = clip_y0 = 0;
    clip_x1 = kW;
    clip_y1 = kH;
  }

  // A pixel in cart coordinates (the camera applied here), clipped, through the palette map.
  __attribute__((always_inline)) inline void px(int x, int y, int c) {
    x -= cam_x;
    y -= cam_y;
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) fb[y * kW + x] = pmap[c];
  }
  // A row span x0..x1 inclusive (cart coordinates), clipped.
  __attribute__((always_inline)) inline void hspan(int x0, int x1, int y, int c) {
    y -= cam_y;
    if (y < clip_y0 || y >= clip_y1) return;
    x0 = std::max(x0 - cam_x, clip_x0);
    x1 = std::min(x1 - cam_x, clip_x1 - 1);
    if (x1 >= x0) std::memset(fb + y * kW + x0, pmap[c], static_cast<size_t>(x1 - x0 + 1));
  }
  void line(int x0, int y0, int x1, int y1, int c) {
    const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    // Wholly off one side of the clip: nothing to draw (the steps would all be wasted).
    const int ax0 = x0 - cam_x, ax1 = x1 - cam_x, ay0 = y0 - cam_y, ay1 = y1 - cam_y;
    if ((ax0 < clip_x0 && ax1 < clip_x0) || (ax0 >= clip_x1 && ax1 >= clip_x1) || (ay0 < clip_y0 && ay1 < clip_y0) ||
        (ay0 >= clip_y1 && ay1 >= clip_y1))
      return;
    int err = dx + dy;
    for (;;) {
      px(x0, y0, c);
      if (x0 == x1 && y0 == y1) break;
      const int e2 = 2 * err;
      if (e2 >= dy) {
        err += dy;
        x0 += sx;
      }
      if (e2 <= dx) {
        err += dx;
        y0 += sy;
      }
    }
  }
  // check.py's circles: filled where dx²+dy² <= r²+r; the outline where r²-r <= dx²+dy² <= r²+r.
  void circle(int x, int y, int r, int c, bool fill) {
    if (r < 0) return;
    const int outer = r * r + r, inner = r * r - r;
    for (int dy = -r; dy <= r; ++dy) {
      const int rest = outer - dy * dy;
      if (rest < 0) continue;
      int b = static_cast<int>(std::sqrt(static_cast<float>(rest)));
      while (b * b > rest) --b;
      while ((b + 1) * (b + 1) <= rest) ++b;
      b = std::min(b, r);
      if (fill) {
        hspan(x - b, x + b, y + dy, c);
        continue;
      }
      const int need = inner - dy * dy;  // dx² at least this
      int a = 0;
      if (need > 0) {
        a = static_cast<int>(std::sqrt(static_cast<float>(need)));
        while (a * a < need) ++a;
        while (a > 0 && (a - 1) * (a - 1) >= need) --a;
      }
      if (a > b) continue;
      hspan(x + a, x + b, y + dy, c);
      hspan(x - b, x - a, y + dy, c);
    }
  }
  int print(const char* s, size_t n, int x, int y, int c) {
    const int x_start = x;
    for (size_t i = 0; i < n; ++i) {
      const unsigned char ch = static_cast<unsigned char>(s[i]);
      if (ch == '\n') {
        y += 6;
        x = x_start;
        continue;
      }
      if ((ch & 0xC0) == 0x80) continue;  // UTF-8: one box a character, not a byte
      const uint16_t g = ch < 128 ? kGlyphs.g[ch >= 'a' && ch <= 'z' ? ch - 32 : ch] : kBox;
      for (int k = 0; k < 15; ++k)
        if (g & (1u << (14 - k))) px(x + k % 3, y + k / 3, c);
      x += 4;
    }
    return x;
  }
  float rnd01() {  // xorshift32, 24 bits of it
    rng ^= rng << 13;
    rng ^= rng >> 17;
    rng ^= rng << 5;
    return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
  }
};

namespace {

Cart::Impl* S = nullptr;  // the running cart (one at a time)

// Argument i of a C function, straight off the stack (nil when absent).
__attribute__((always_inline)) inline const TValue* arg(lua_State* L, int i) {
  const StkId o = L->ci->func.p + i;
  return o < L->top.p ? s2v(o) : &G(L)->nilvalue;
}
__attribute__((always_inline)) inline bool absent(lua_State* L, int i) { return ttisnil(arg(L, i)); }
// Coordinates: floored, held to a range where the arithmetic can't overflow. Numbers are read
// in place; anything else goes the long way (a string that converts, or the error).
__attribute__((always_inline)) inline int coord(lua_State* L, int i) {
  const TValue* o = arg(L, i);
  if (ttisinteger(o)) return static_cast<int>(std::clamp<lua_Integer>(ivalue(o), -32768, 32767));
  const lua_Number v = ttisfloat(o) ? fltvalue(o) : luaL_checknumber(L, i);
  if (!(v > -32768.0f)) return -32768;  // NaN too
  if (v > 32767.0f) return 32767;
  const int t = static_cast<int>(v);  // truncates; floor is one less below zero
  return t - (v < static_cast<lua_Number>(t));
}
inline int opt_coord(lua_State* L, int i, int d) { return absent(L, i) ? d : coord(L, i); }
// The color argument: absent, the last one used; given, it becomes the last one used.
__attribute__((always_inline)) inline int color_arg(lua_State* L, int i) {
  if (absent(L, i)) return S->color;
  S->color = coord(L, i) & 15;
  return S->color;
}
// A number as check.py writes it in a color's error: 0x1000000, -0x1 (Lua's own formatter has no %x).
std::string hex_of(lua_Integer v) {
  char b[24];
  std::snprintf(b, sizeof b, "%s0x%llx", v < 0 ? "-" : "", static_cast<unsigned long long>(v < 0 ? -v : v));
  return b;
}
inline void push_num(lua_State* L, float v) {
  // Whole numbers as integers (faster arithmetic in the cart, and "3" not "3.0" when printed).
  if (v >= -2147483520.0f && v <= 2147483520.0f && v == std::floor(v)) lua_pushinteger(L, static_cast<lua_Integer>(v));
  else lua_pushnumber(L, v);
}

int f_cls(lua_State* L) {
  const int c = lua_isnoneornil(L, 1) ? 0 : coord(L, 1) & 15;
  std::memset(S->fb, c, kW * kH);
  return 0;
}
int f_pset(lua_State* L) {
  const int x = coord(L, 1), y = coord(L, 2);
  S->px(x, y, color_arg(L, 3));
  return 0;
}
int f_pget(lua_State* L) {
  const int x = coord(L, 1), y = coord(L, 2);
  lua_pushinteger(L, x >= 0 && x < kW && y >= 0 && y < kH ? S->fb[y * kW + x] : 0);
  return 1;
}
int f_line(lua_State* L) {
  const int x0 = coord(L, 1), y0 = coord(L, 2), x1 = coord(L, 3), y1 = coord(L, 4);
  S->line(x0, y0, x1, y1, color_arg(L, 5));
  return 0;
}
int f_rect(lua_State* L) {
  const int x0 = coord(L, 1), y0 = coord(L, 2), x1 = coord(L, 3), y1 = coord(L, 4), c = color_arg(L, 5);
  S->line(x0, y0, x1, y0, c);
  S->line(x1, y0, x1, y1, c);
  S->line(x1, y1, x0, y1, c);
  S->line(x0, y1, x0, y0, c);
  return 0;
}
int f_rectfill(lua_State* L) {
  int x0 = coord(L, 1), y0 = coord(L, 2), x1 = coord(L, 3), y1 = coord(L, 4);
  const int c = color_arg(L, 5);
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  for (int y = std::max(y0, S->cam_y + S->clip_y0), ye = std::min(y1, S->cam_y + S->clip_y1 - 1); y <= ye; ++y)
    S->hspan(x0, x1, y, c);
  return 0;
}
int f_circ(lua_State* L) {
  const int x = coord(L, 1), y = coord(L, 2), r = coord(L, 3);
  S->circle(x, y, r, color_arg(L, 4), false);
  return 0;
}
int f_circfill(lua_State* L) {
  const int x = coord(L, 1), y = coord(L, 2), r = coord(L, 3);
  S->circle(x, y, r, color_arg(L, 4), true);
  return 0;
}
int f_print(lua_State* L) {
  // The arguments first: the text conversion pushes onto the stack (an absent 4th would read it).
  const int x = opt_coord(L, 2, 0), y = opt_coord(L, 3, 0), c = color_arg(L, 4);
  size_t n = 0;
  const char* s = "";
  if (lua_type(L, 1) == LUA_TNUMBER && !lua_isinteger(L, 1)) {  // whole numbers print without ".0"
    const lua_Number v = lua_tonumber(L, 1);
    if (v == std::floor(v) && std::fabs(v) < 1e9f) s = lua_pushfstring(L, "%d", static_cast<int>(v));
  }
  if (!*s && !lua_isnoneornil(L, 1)) s = luaL_tolstring(L, 1, &n);
  else n = std::strlen(s);
  lua_pushinteger(L, S->print(s, n, x, y, c));
  return 1;
}
// Sprite n's 64 pixels, from the SPRITES table at stack index `sprites`: its digits row by row;
// spaces and line breaks between them are for reading (API 4). Decoded once, again only when the
// digits change (a number outside 0-255 goes through `tmp`). nullptr when it isn't 64 hex digits.
const uint8_t* sprite_pixels(lua_State* L, int sprites, int n, Cart::Impl::Sprite& tmp) {
  lua_rawgeti(L, sprites, n);
  size_t len = 0;
  const char* s = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
  char hex[64];
  int got = 0;
  for (size_t i = 0; s && i < len; ++i) {
    const char h = s[i];
    if (h == ' ' || h == '\n' || h == '\t' || h == '\r') continue;
    const bool digit = (h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F');
    if (!digit || got == 64) {
      got = -1;
      break;
    }
    hex[got++] = h;
  }
  lua_pop(L, 1);
  if (got != 64) return nullptr;
  Cart::Impl::Sprite* sp = n >= 0 && n < 256 ? &S->sprites[n] : &tmp;
  if (std::memcmp(sp->hex, hex, 64) != 0) {
    for (int i = 0; i < 64; ++i) {
      const char h = hex[i];
      sp->px[i] = static_cast<uint8_t>(h <= '9' ? h - '0' : (h | 0x20) - 'a' + 10);
    }
    std::memcpy(sp->hex, hex, 64);
  }
  return sp->px;
}
// The sprites of a w x h block from n (n, n + 1, ... row by row, w across), into got[]: for spr's and
// sspr's block forms. Each must be one of 0-255 (they're kept decoded; tmp holds only one).
void block_pixels(lua_State* L, const char* who, int sprites, int n, int w, int h, const uint8_t** got) {
  if (w < 1 || h < 1 || w * h > 256) luaL_error(L, "%s: a block is 1 to 256 sprites (%d x %d)", who, w, h);
  Cart::Impl::Sprite tmp;
  for (int i = 0; i < w * h; ++i) {
    const int k = n + i;
    if (k < 0 || k > 255) luaL_error(L, "%s: the block from %d runs to sprite %d; sprites are 0-255", who, n, n + w * h - 1);
    if (!(got[i] = sprite_pixels(L, sprites, k, tmp)))
      luaL_error(L, "%s: SPRITES[%d] must be 64 hex digits (spaces between rows are fine)", who, k);
  }
}
// One sprite's 64 pixels at (x, y), flipped as asked, leaving out the clear colors (palt).
inline void blit8(const uint8_t* p, int x, int y, bool fx, bool fy) {
  const unsigned clear = S->clear;
  for (int i = 0; i < 64; ++i) {
    const int v = p[i];
    if (clear >> v & 1) continue;
    S->px(x + (fx ? 7 - (i & 7) : i & 7), y + (fy ? 7 - (i >> 3) : i >> 3), v);
  }
}
// spr(n, x, y, [fx], [fy]): sprite n at (x, y). spr(n, x, y, w, h, [fx], [fy]) (API 6, a number
// fourth): the w x h block of sprites from n, n + 1, ... row by row; flipping flips the whole block.
int f_spr(lua_State* L) {
  const int n = coord(L, 1), x = coord(L, 2), y = coord(L, 3);
  const bool block = lua_type(L, 4) == LUA_TNUMBER;
  const int w = block ? coord(L, 4) : 1, h = block ? opt_coord(L, 5, 1) : 1;
  const bool fx = lua_toboolean(L, block ? 6 : 4), fy = lua_toboolean(L, block ? 7 : 5);
  if (lua_getglobal(L, "SPRITES") != LUA_TTABLE) return luaL_error(L, "spr: no SPRITES table");
  const int sprites = lua_gettop(L);
  if (!block) {
    Cart::Impl::Sprite tmp;
    const uint8_t* p = sprite_pixels(L, sprites, n, tmp);
    if (!p) return luaL_error(L, "spr: SPRITES[%d] must be 64 hex digits (spaces between rows are fine)", n);
    blit8(p, x, y, fx, fy);
  } else {
    const uint8_t* got[256];
    block_pixels(L, "spr", sprites, n, w, h, got);
    for (int r = 0; r < h; ++r)
      for (int c = 0; c < w; ++c)
        blit8(got[r * w + c], x + (fx ? w - 1 - c : c) * 8, y + (fy ? h - 1 - r : r) * 8, fx, fy);
  }
  lua_pop(L, 1);
  return 0;
}
// sspr(n, x, y, dw, dh, [fx], [fy], [w], [h]) (API 6): sprite n (or the w x h block from it) stretched
// to dw x dh pixels at (x, y), nearest pixel: the pixel at (i, j) of the box is the source's
// (i * sw // dw, j * sh // dh), sw and sh the source's size in pixels.
int f_sspr(lua_State* L) {
  const int n = coord(L, 1), x = coord(L, 2), y = coord(L, 3), dw = coord(L, 4), dh = coord(L, 5);
  const bool fx = lua_toboolean(L, 6), fy = lua_toboolean(L, 7);
  const int w = opt_coord(L, 8, 1), h = opt_coord(L, 9, 1);
  if (lua_getglobal(L, "SPRITES") != LUA_TTABLE) return luaL_error(L, "sspr: no SPRITES table");
  const uint8_t* got[256];
  block_pixels(L, "sspr", lua_gettop(L), n, w, h, got);
  const int sw = w * 8, sh = h * 8;
  // Only the box's part the clip shows (the camera moves the box). The clip lies inside the
  // picture, so those pixels are written straight in; the products stay under 2^31 (dw, dh are
  // coordinates, sw, sh at most 2048), so 32-bit division: a 64-bit one is a library call a pixel
  // on the board. Each column's source column is worked out once.
  const int i0 = std::max(0, S->clip_x0 + S->cam_x - x), i1 = std::min(dw, S->clip_x1 + S->cam_x - x);
  const int j0 = std::max(0, S->clip_y0 + S->cam_y - y), j1 = std::min(dh, S->clip_y1 + S->cam_y - y);
  if (i0 >= i1 || j0 >= j1) {
    lua_pop(L, 1);
    return 0;
  }
  int16_t col[kW];  // i1 - i0 <= the clip's width
  for (int i = i0; i < i1; ++i) {
    const int u = i * sw / dw;
    col[i - i0] = static_cast<int16_t>(fx ? sw - 1 - u : u);
  }
  const unsigned clear = S->clear;
  const uint8_t* pmap = S->pmap;
  for (int j = j0; j < j1; ++j) {
    int v = j * sh / dh;
    if (fy) v = sh - 1 - v;
    const uint8_t* const* srow = got + (v >> 3) * w;
    const int vr = (v & 7) * 8;
    uint8_t* out = S->fb + (y + j - S->cam_y) * kW + (x - S->cam_x);
    for (int i = i0; i < i1; ++i) {
      const int u = col[i - i0];
      const int c = srow[u >> 3][vr + (u & 7)];
      if (!(clear >> c & 1)) out[i] = pmap[c];
    }
  }
  lua_pop(L, 1);
  return 0;
}
// palt(c, on) (API 6): whether color c is left out of spr(), sspr() and map() (color 0 is, to start
// with); palt(): only color 0 again.
int f_palt(lua_State* L) {
  if (absent(L, 1)) {
    S->clear = 1;
    return 0;
  }
  const int c = coord(L, 1);
  if (c < 0 || c > 15) return luaL_error(L, "palt: color %d isn't one of 0-15", c);
  if (lua_type(L, 2) != LUA_TBOOLEAN) return luaL_error(L, "palt: give a color and true (left out) or false (drawn)");
  if (lua_toboolean(L, 2)) S->clear = static_cast<uint16_t>(S->clear | 1u << c);
  else S->clear = static_cast<uint16_t>(S->clear & ~(1u << c));
  return 0;
}
// ---- sprite flags (API 6) -----------------------------------------------------------------------
// FLAGS is the cart's own table, sprite -> a number whose bits 0-7 are its flags; a sprite not in
// it has none. Read and written in place, so the cart, the editors and these see the same thing.
int flags_of(lua_State* L, const char* who, int flags, int n) {
  if (flags == 0) return 0;  // no FLAGS table
  lua_rawgeti(L, flags, n);
  int isnum = 0;
  const lua_Integer v = lua_tointegerx(L, -1, &isnum);
  const bool none = lua_isnil(L, -1);
  lua_pop(L, 1);
  if (none) return 0;
  if (!isnum || v < 0 || v > 255) luaL_error(L, "%s: FLAGS[%d] must be a whole number 0-255 (a bit a flag)", who, n);
  return static_cast<int>(v);
}
int flag_arg(lua_State* L, const char* who, int i) {
  const int f = coord(L, i);
  if (f < 0 || f > 7) luaL_error(L, "%s: flag %d isn't one of 0-7", who, f);
  return f;
}
// fget(n): sprite n's flags as a number; fget(n, f): whether it has flag f.
int f_fget(lua_State* L) {
  // The arguments first: FLAGS goes on the stack after them (an absent 2nd would read it).
  const int n = coord(L, 1), f = absent(L, 2) ? -1 : flag_arg(L, "fget", 2);
  const int flags = lua_getglobal(L, "FLAGS") == LUA_TTABLE ? lua_gettop(L) : 0;
  const int v = flags_of(L, "fget", flags, n);
  if (f < 0) lua_pushinteger(L, v);
  else lua_pushboolean(L, v >> f & 1);
  return 1;
}
// fset(n, f, on): flag f of sprite n on or off; fset(n, v): all of them, v's bits 0-7. Makes FLAGS
// when the cart has none.
int f_fset(lua_State* L) {
  const int n = coord(L, 1);
  const bool all = absent(L, 3);  // fset(n, v), before FLAGS goes on the stack
  const int a = all ? coord(L, 2) : flag_arg(L, "fset", 2);
  const bool on = lua_toboolean(L, 3);
  if (all && (a < 0 || a > 255)) return luaL_error(L, "fset: %d isn't 0-255 (a bit a flag)", a);
  if (lua_getglobal(L, "FLAGS") != LUA_TTABLE) {
    lua_pop(L, 1);
    lua_newtable(L);
    lua_pushvalue(L, -1);
    lua_setglobal(L, "FLAGS");
  }
  const int flags = lua_gettop(L);
  int v = a;
  if (!all) {
    v = flags_of(L, "fset", flags, n);
    v = on ? v | 1 << a : v & ~(1 << a);
  }
  lua_pushinteger(L, v);
  lua_rawseti(L, flags, n);
  lua_pop(L, 1);
  return 0;
}

// ---- the map (API 5) ----------------------------------------------------------------------------
// MAP is a list of strings, a row each from the top, a character a cell: "." empty, else a sprite,
// 0-9 a-z A-Z + / for 0 to 63. Up to 128 cells wide and 64 tall; shorter rows end in empty cells.
int tile_of(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'z') return c - 'a' + 10;
  if (c >= 'A' && c <= 'Z') return c - 'A' + 36;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}
// The map, read from MAP the first time and whenever MAP is another table (a cart's next level):
// the table read is kept in the registry, so it can't be collected and its address reused.
void read_map(lua_State* L, const char* who) {
  static const char kKey = 0;  // its registry key: this byte's address
  if (lua_getglobal(L, "MAP") != LUA_TTABLE) luaL_error(L, "%s: no MAP table", who);
  lua_rawgetp(L, LUA_REGISTRYINDEX, &kKey);
  const bool same = S->map_read && lua_rawequal(L, -1, -2);
  lua_pop(L, 1);
  if (same) {
    lua_pop(L, 1);
    return;
  }
  const lua_Unsigned rows = lua_rawlen(L, -1);
  if (rows > static_cast<lua_Unsigned>(Cart::Impl::kMapH))
    luaL_error(L, "%s: MAP has %d rows; a map has at most %d", who, static_cast<int>(rows), Cart::Impl::kMapH);
  std::memset(S->map_cells, Cart::Impl::kNoTile, sizeof S->map_cells);
  int w = 0;
  for (int r = 0; r < static_cast<int>(rows); ++r) {
    lua_rawgeti(L, -1, r + 1);
    size_t len = 0;
    const char* row = lua_type(L, -1) == LUA_TSTRING ? lua_tolstring(L, -1, &len) : nullptr;
    if (!row) luaL_error(L, "%s: MAP[%d] isn't a string of cells", who, r + 1);
    if (len > static_cast<size_t>(Cart::Impl::kMapW))
      luaL_error(L, "%s: MAP[%d] is %d cells wide; a map has at most %d", who, r + 1, static_cast<int>(len), Cart::Impl::kMapW);
    for (size_t c = 0; c < len; ++c) {
      if (row[c] == '.') continue;
      const int t = tile_of(row[c]);
      if (t < 0)
        luaL_error(L, "%s: MAP[%d] has '%c' at cell %d; a cell is '.' or a sprite, 0-9 a-z A-Z + / for 0 to 63",
                   who, r + 1, row[c], static_cast<int>(c));
      S->map_cells[r * Cart::Impl::kMapW + static_cast<int>(c)] = static_cast<uint8_t>(t);
    }
    w = std::max(w, static_cast<int>(len));
    lua_pop(L, 1);
  }
  S->map_w = w;
  S->map_h = static_cast<int>(rows);
  S->map_read = true;
  lua_rawsetp(L, LUA_REGISTRYINDEX, &kKey);  // pops MAP
}
// map([cx, cy, sx, sy, cw, ch, layer]): cw x ch cells from cell (cx, cy) drawn as sprites with cell
// (cx, cy)'s top left at (sx, sy); the clear colors (palt; color 0) are see-through, as spr() draws.
// With nothing, the whole map at (0, 0). With a layer (API 6), only the cells whose sprite has one of
// those flags (fget(n) & layer ~= 0); 0 is every cell. Only the cells the camera and clip can show
// are drawn.
int f_map(lua_State* L) {
  read_map(L, "map");
  const int cx = opt_coord(L, 1, 0), cy = opt_coord(L, 2, 0), sx = opt_coord(L, 3, 0), sy = opt_coord(L, 4, 0);
  const int cw = opt_coord(L, 5, S->map_w), ch = opt_coord(L, 6, S->map_h);
  const int layer = opt_coord(L, 7, 0);
  // The cells on screen: in the map, in the asked-for block, and with some of their 8 x 8 inside the
  // clip once the camera has moved them (cell c's left edge is at sx + (c - cx) * 8 - cam_x).
  const auto floor8 = [](int v) { return v >= 0 ? v / 8 : -((-v + 7) / 8); };
  const int c0 = std::max({cx, 0, cx + floor8(S->clip_x0 + S->cam_x - sx - 8) + 1});
  const int c1 = std::min({cx + cw, S->map_w, cx + floor8(S->clip_x1 + S->cam_x - sx + 7)});
  const int r0 = std::max({cy, 0, cy + floor8(S->clip_y0 + S->cam_y - sy - 8) + 1});
  const int r1 = std::min({cy + ch, S->map_h, cy + floor8(S->clip_y1 + S->cam_y - sy + 7)});
  if (c0 >= c1 || r0 >= r1) return 0;
  if (lua_getglobal(L, "SPRITES") != LUA_TTABLE) return luaL_error(L, "map: no SPRITES table");
  const int sprites = lua_gettop(L);
  const int flags = layer && lua_getglobal(L, "FLAGS") == LUA_TTABLE ? lua_gettop(L) : 0;
  const uint8_t* got[64] = {};  // each sprite (and with a layer, whether it's in it) looked up once a call
  int8_t shown[64];
  std::memset(shown, -1, sizeof shown);
  Cart::Impl::Sprite tmp;
  for (int r = r0; r < r1; ++r) {
    const uint8_t* row = S->map_cells + r * Cart::Impl::kMapW;
    const int y = sy + (r - cy) * 8;
    for (int c = c0; c < c1; ++c) {
      const int t = row[c];
      if (t == Cart::Impl::kNoTile) continue;
      if (layer) {
        if (shown[t] < 0) shown[t] = (flags_of(L, "map", flags, t) & layer) != 0;
        if (!shown[t]) continue;
      }
      if (!got[t] && !(got[t] = sprite_pixels(L, sprites, t, tmp)))
        return luaL_error(L, "map: cell (%d, %d) is sprite %d, and SPRITES[%d] isn't 64 hex digits", c, r, t, t);
      blit8(got[t], sx + (c - cx) * 8, y, false, false);
    }
  }
  lua_settop(L, sprites - 1);
  return 0;
}
// tline(x0, y0, x1, y1, mx, my, [mdx, mdy, layer]) (API 7): the line from (x0, y0) to (x1, y1), its
// pixels taken from the map: the first from map point (mx, my), in cells (fractions are inside the
// cell: x 2.5 is sprite column 4 of cell 2), the next a step (mdx, mdy) on (1/8, 0: the map at its
// size). Empty cells, cells off the map and clear colors leave the pixel. The line's pixels are
// line()'s. The map points are fixed point, 16 bits of fraction (check.py's are too), so the steps
// add up the same everywhere. A row a line, mdx and mdy changing with the row: a floor in
// perspective (libs/mode7.lua), a map turned and zoomed.
int f_tline(lua_State* L) {
  read_map(L, "tline");
  int x0 = coord(L, 1), y0 = coord(L, 2);
  const int x1 = coord(L, 3), y1 = coord(L, 4);
  const auto fixed = [&](int i, double d) -> int64_t {
    const double v = absent(L, i) ? d : static_cast<double>(luaL_checknumber(L, i));
    if (!(v > -1e6 && v < 1e6)) return 0;  // NaN, or a point no map reaches
    return static_cast<int64_t>(std::floor(v * 65536.0));
  };
  int64_t mx = fixed(5, 0), my = fixed(6, 0);
  const int64_t dmx = fixed(7, 0.125), dmy = fixed(8, 0);
  const int layer = opt_coord(L, 9, 0);
  const int ax0 = x0 - S->cam_x, ax1 = x1 - S->cam_x, ay0 = y0 - S->cam_y, ay1 = y1 - S->cam_y;
  if ((ax0 < S->clip_x0 && ax1 < S->clip_x0) || (ax0 >= S->clip_x1 && ax1 >= S->clip_x1) ||
      (ay0 < S->clip_y0 && ay1 < S->clip_y0) || (ay0 >= S->clip_y1 && ay1 >= S->clip_y1))
    return 0;
  if (lua_getglobal(L, "SPRITES") != LUA_TTABLE) return luaL_error(L, "tline: no SPRITES table");
  const int sprites = lua_gettop(L);
  const int flags = layer && lua_getglobal(L, "FLAGS") == LUA_TTABLE ? lua_gettop(L) : 0;
  const uint8_t* got[64] = {};
  int8_t shown[64];
  std::memset(shown, -1, sizeof shown);
  Cart::Impl::Sprite tmp;
  const unsigned clear = S->clear;
  const int64_t mw = static_cast<int64_t>(S->map_w) << 16, mh = static_cast<int64_t>(S->map_h) << 16;
  // A row left to right (the floor in perspective: a hundred of these a frame) the quick way, with
  // the same pixels: clipped first, 32-bit sums (64-bit ones are slow on the S3), and the map read
  // again only where the line moves into another cell.
  if (y0 == y1 && x0 <= x1) {
    const int y = y0 - S->cam_y;
    int xa = x0 - S->cam_x, xb = x1 - S->cam_x;
    if (y < S->clip_y0 || y >= S->clip_y1) return 0;
    if (xa < S->clip_x0) {
      mx += dmx * (S->clip_x0 - xa);
      my += dmy * (S->clip_x0 - xa);
      xa = S->clip_x0;
    }
    xb = std::min(xb, S->clip_x1 - 1);
    const int64_t n = xb - xa;
    const auto fits = [](int64_t v) { return v > -(int64_t{1} << 30) && v < (int64_t{1} << 30); };
    if (xb >= xa && fits(mx) && fits(my) && fits(mx + dmx * n) && fits(my + dmy * n) && fits(dmx) && fits(dmy)) {
      int32_t ux = static_cast<int32_t>(mx), uy = static_cast<int32_t>(my);
      const int32_t sx = static_cast<int32_t>(dmx), sy = static_cast<int32_t>(dmy);
      const uint32_t w32 = static_cast<uint32_t>(mw), h32 = static_cast<uint32_t>(mh);
      uint8_t* row = S->fb + y * kW;
      const uint8_t* pm = S->pmap;
      int last = -1;
      const uint8_t* p = nullptr;
      for (int x = xa; x <= xb; ++x, ux += sx, uy += sy) {
        if (static_cast<uint32_t>(ux) >= w32 || static_cast<uint32_t>(uy) >= h32) continue;
        const int cell = (uy >> 16) * Cart::Impl::kMapW + (ux >> 16);
        if (cell != last) {
          last = cell;
          const int t = S->map_cells[cell];
          p = nullptr;
          if (t != Cart::Impl::kNoTile) {
            if (layer && shown[t] < 0) shown[t] = (flags_of(L, "tline", flags, t) & layer) != 0;
            if (!layer || shown[t]) {
              if (!got[t] && !(got[t] = sprite_pixels(L, sprites, t, tmp)))
                return luaL_error(L, "tline: cell (%d, %d) is sprite %d, and SPRITES[%d] isn't 64 hex digits", ux >> 16, uy >> 16, t, t);
              p = got[t];
            }
          }
        }
        if (!p) continue;
        const int v = p[((uy >> 13) & 7) * 8 + ((ux >> 13) & 7)];
        if (!(clear >> v & 1)) row[x] = pm[v];
      }
      lua_settop(L, sprites - 1);
      return 0;
    }
    // (map points far out of reach: the long way, from the line's own start)
    mx = fixed(5, 0);
    my = fixed(6, 0);
  }
  const int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
  const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  for (;;) {
    if (mx >= 0 && my >= 0 && mx < mw && my < mh) {
      const int c = static_cast<int>(mx >> 16), r = static_cast<int>(my >> 16);
      const int t = S->map_cells[r * Cart::Impl::kMapW + c];
      if (t != Cart::Impl::kNoTile) {
        bool draw = true;
        if (layer) {
          if (shown[t] < 0) shown[t] = (flags_of(L, "tline", flags, t) & layer) != 0;
          draw = shown[t];
        }
        if (draw) {
          if (!got[t] && !(got[t] = sprite_pixels(L, sprites, t, tmp)))
            return luaL_error(L, "tline: cell (%d, %d) is sprite %d, and SPRITES[%d] isn't 64 hex digits", c, r, t, t);
          const int v = got[t][(static_cast<int>(my >> 13) & 7) * 8 + (static_cast<int>(mx >> 13) & 7)];
          if (!(clear >> v & 1)) S->px(x0, y0, v);
        }
      }
    }
    if (x0 == x1 && y0 == y1) break;
    const int e2 = 2 * err;
    if (e2 >= dy) {
      err += dy;
      x0 += sx;
    }
    if (e2 <= dx) {
      err += dx;
      y0 += sy;
    }
    mx += dmx;
    my += dmy;
  }
  lua_settop(L, sprites - 1);
  return 0;
}
// mget(cx, cy): the sprite in that cell, or nil for an empty cell or one off the map.
int f_mget(lua_State* L) {
  read_map(L, "mget");
  const int c = coord(L, 1), r = coord(L, 2);
  if (c < 0 || r < 0 || c >= S->map_w || r >= S->map_h) return 0;
  const int t = S->map_cells[r * Cart::Impl::kMapW + c];
  if (t == Cart::Impl::kNoTile) return 0;
  lua_pushinteger(L, t);
  return 1;
}
// mset(cx, cy, n): that cell becomes sprite n (0-63), or empty with nil. The map's size doesn't change.
int f_mset(lua_State* L) {
  read_map(L, "mset");
  const int c = coord(L, 1), r = coord(L, 2);
  if (c < 0 || r < 0 || c >= S->map_w || r >= S->map_h)
    return luaL_error(L, "mset: (%d, %d) is off the map, which is %d x %d cells", c, r, S->map_w, S->map_h);
  int t = Cart::Impl::kNoTile;
  if (!absent(L, 3)) {
    t = coord(L, 3);
    if (t < 0 || t > 63) return luaL_error(L, "mset: %d isn't a sprite of the map's, 0-63", t);
  }
  S->map_cells[r * Cart::Impl::kMapW + c] = static_cast<uint8_t>(t);
  return 0;
}
// msize(): the map's width and height in cells.
int f_msize(lua_State* L) {
  read_map(L, "msize");
  lua_pushinteger(L, S->map_w);
  lua_pushinteger(L, S->map_h);
  return 2;
}
// palette(i, 0xRRGGBB): what color index i shows, from now on (the whole picture, this frame
// included); palette(i): its color now; palette(): the starting 16 again.
int f_palette(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    S->cart->reset_colors();
    return 0;
  }
  const lua_Integer i = luaL_checkinteger(L, 1);
  if (i < 0 || i > 15) return luaL_error(L, "palette: color %d isn't one of 0-15", static_cast<int>(i));
  if (lua_isnoneornil(L, 2)) {
    lua_pushinteger(L, static_cast<lua_Integer>(S->cart->colors()[i]));
    return 1;
  }
  const lua_Integer rgb = luaL_checkinteger(L, 2);
  if (rgb < 0 || rgb > 0xFFFFFF) return luaL_error(L, "palette: %s isn't a 0xRRGGBB color", hex_of(rgb).c_str());
  S->cart->set_color(static_cast<int>(i), static_cast<uint32_t>(rgb));
  return 0;
}

// rowshift(y, dx) (API 7): picture row y shows moved dx pixels right (left when negative), its edge
// pixel run on into the gap; rowshift(y): row y as drawn; rowshift(): every row. Rows keep their
// moves until changed. Only the showing moves: pget() and the next frame see what was drawn.
// Rows off the picture are left alone, as drawing there is.
int f_rowshift(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    for (int y = 0; y < kH; ++y) S->cart->set_shift(y, 0);
    return 0;
  }
  const int y = coord(L, 1), dx = opt_coord(L, 2, 0);
  if (y >= 0 && y < kH) S->cart->set_shift(y, dx);
  return 0;
}
// rowpal(y, c, 0xRRGGBB) (API 7): in picture row y, color c shows as that color (a sky in 128
// shades: a different blue a row); rowpal(y, c): the cart's color again there; rowpal(y): all of
// row y's; rowpal(): every row's. Kept until changed, as palette() is.
int f_rowpal(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    for (int y = 0; y < kH; ++y)
      for (int c = 0; c < 16; ++c) S->cart->set_row_color(y, c, -1);
    return 0;
  }
  const int y = coord(L, 1);
  int c0 = 0, c1 = 15;
  if (!absent(L, 2)) {
    const int c = coord(L, 2);
    if (c < 0 || c > 15) return luaL_error(L, "rowpal: color %d isn't one of 0-15", c);
    c0 = c1 = c;
  }
  int64_t rgb = -1;
  if (!absent(L, 3)) {
    if (absent(L, 2)) return luaL_error(L, "rowpal: give a row, a color and 0xRRGGBB");
    const lua_Integer v = luaL_checkinteger(L, 3);
    if (v < 0 || v > 0xFFFFFF) return luaL_error(L, "rowpal: %s isn't a 0xRRGGBB color", hex_of(v).c_str());
    rgb = v;
  }
  if (y < 0 || y >= kH) return 0;
  for (int c = c0; c <= c1; ++c) S->cart->set_row_color(y, c, rgb);
  return 0;
}

int f_pal(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    for (int i = 0; i < 16; ++i) S->pmap[i] = static_cast<uint8_t>(i);
  } else {
    S->pmap[coord(L, 1) & 15] = static_cast<uint8_t>(coord(L, 2) & 15);
  }
  return 0;
}
int f_camera(lua_State* L) {
  S->cam_x = opt_coord(L, 1, 0);
  S->cam_y = opt_coord(L, 2, 0);
  return 0;
}
int f_clip(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    S->clip_x0 = S->clip_y0 = 0;
    S->clip_x1 = kW;
    S->clip_y1 = kH;
  } else {
    const int x = coord(L, 1), y = coord(L, 2), w = coord(L, 3), h = coord(L, 4);
    S->clip_x0 = std::clamp(x, 0, kW);
    S->clip_y0 = std::clamp(y, 0, kH);
    S->clip_x1 = std::clamp(x + w, S->clip_x0, kW);
    S->clip_y1 = std::clamp(y + h, S->clip_y0, kH);
  }
  return 0;
}

int f_btn(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    lua_pushinteger(L, S->in.held);
    return 1;
  }
  const int b = coord(L, 1);
  lua_pushboolean(L, b >= 0 && b < kButtons && (S->in.held >> b & 1));
  return 1;
}
int f_btnp(lua_State* L) {
  const int b = coord(L, 1);
  lua_pushboolean(L, b >= 0 && b < kButtons && (S->in.held >> b & 1) && !(S->was.held >> b & 1));
  return 1;
}
// touch(): the picture's pixel under the finger; touch(true): the canvas's (3x finer).
int f_touch(lua_State* L) {
  if (!S->in.touching) {
    lua_pushnil(L);
    return 1;
  }
  if (!lua_toboolean(L, 1)) {
    push_num(L, std::floor(S->in.tx));
    push_num(L, std::floor(S->in.ty));
  } else if (S->full) {  // glass pixels
    push_num(L, std::floor(S->in.tx * 3 + S->caps.window_x));
    push_num(L, std::floor(S->in.ty * 3 + S->caps.window_y));
  } else {
    push_num(L, std::floor(S->in.tx * 3));
    push_num(L, std::floor(S->in.ty * 3));
  }
  return 2;
}
int f_caps(lua_State* L) {
  const Caps& c = S->caps;
  lua_createtable(L, 0, 16);
  auto num = [&](const char* k, lua_Number v) {
    lua_pushnumber(L, v);
    lua_setfield(L, -2, k);
  };
  auto boolean = [&](const char* k, bool v) {
    lua_pushboolean(L, v);
    lua_setfield(L, -2, k);
  };
  lua_pushinteger(L, S->full ? c.glass_w : c.w);  // the canvas: the window, or the glass
  lua_setfield(L, -2, "w");
  lua_pushinteger(L, S->full ? c.glass_h : c.h);
  lua_setfield(L, -2, "h");
  lua_pushinteger(L, S->full ? c.window_x : 0);  // where the 128 x 128 picture sits on it (x 3)
  lua_setfield(L, -2, "picture_x");
  lua_pushinteger(L, S->full ? c.window_y : 0);
  lua_setfield(L, -2, "picture_y");
  lua_pushinteger(L, S->full ? c.corner : 0);  // its corners' radius (the glass's), for a full canvas
  lua_setfield(L, -2, "corner");
  num("scale", c.scale);
  boolean("round", c.round);
  boolean("touch", c.touch);
  boolean("tilt", c.tilt);
  boolean("buzz", c.buzz);
  boolean("sound", c.sound);
  boolean("hd", c.hd);
  boolean("stage", c.stage);
  boolean("host", c.host);
  lua_pushinteger(L, kApiVersion);
  lua_setfield(L, -2, "version");
  lua_pushlstring(L, c.board.data(), c.board.size());
  lua_setfield(L, -2, "board");
  lua_newtable(L);  // the port's own calls (Host::extra_api), by name
  int n = 0;
  for (const luaL_Reg* r = S->host->extra_api(); r && r->name; ++r) {
    lua_pushstring(L, r->name);
    lua_rawseti(L, -2, ++n);
  }
  lua_setfield(L, -2, "extras");
  return 1;
}
int f_tilt(lua_State* L) {
  lua_pushnumber(L, S->in.ax);
  lua_pushnumber(L, S->in.ay);
  return 2;
}

inline Host& host_of(lua_State*) { return *S->host; }

int f_buzz(lua_State* L) {
  host_of(L).buzz(std::clamp(coord(L, 1), 10, 500));
  return 0;
}
// TABLE[i] when it's a string (it lives as long as the table keeps it), else null.
const char* text_at(lua_State* L, const char* table, int i) {
  const char* s = nullptr;
  const int top = lua_gettop(L);
  if (lua_getglobal(L, table) == LUA_TTABLE && lua_rawgeti(L, -1, i) == LUA_TSTRING) s = lua_tostring(L, -1);
  lua_settop(L, top);
  return s;
}

// sfx(n): 0-7 built in, 8-63 the cart's SOUNDS, -1 stops the cart's.
int f_sfx(lua_State* L) {
  const int n = coord(L, 1);
  if (n == -1) {
    host_of(L).sound(nullptr);
    return 0;
  }
  if (n >= 0 && n < kSfxCount) {
    host_of(L).sfx(n);
    return 0;
  }
  if (n < kSoundFirst || n > kSoundLast) return luaL_error(L, "sfx(%d): sounds are 0-7 (built in) and 8-63 (your SOUNDS); -1 stops yours", n);
  const char* text = text_at(L, "SOUNDS", n);
  if (!text) return luaL_error(L, "sfx(%d): SOUNDS[%d] isn't there (a string from the sound editor)", n, n);
  Cart::Impl::OwnSound& own = S->sounds[n];
  if (!own.sound || own.text != text) {
    char why[128] = "";
    {  // luaL_error doesn't unwind C++: nothing with a destructor may be alive when it's called
      auto sound = std::make_shared<Sound>();
      const std::string w = parse_sound(text, *sound);
      if (w.empty()) {
        own.text = text;
        own.sound = std::move(sound);
      } else {
        std::snprintf(why, sizeof why, "%s", w.c_str());
      }
    }
    if (*why) return luaL_error(L, "sfx(%d): SOUNDS[%d] %s", n, n, why);
  }
  host_of(L).sound(own.sound);
  return 0;
}

// music(n): the cart's song from MUSIC[n] on; -1 stops it.
int f_music(lua_State* L) {
  const int n = coord(L, 1);
  if (n == -1) {
    host_of(L).music(nullptr);
    return 0;
  }
  if (n < 0 || n >= kPatterns) return luaL_error(L, "music(%d): patterns are 0-63; -1 stops the song", n);
  char why[160] = "";
  {
    std::shared_ptr<const Song> song;
    const std::string w = build_song(n, [L](int i) { return text_at(L, "MUSIC", i); }, [L](int i) { return text_at(L, "SOUNDS", i); }, song);
    if (w.empty()) host_of(L).music(std::move(song));
    else std::snprintf(why, sizeof why, "%s", w.c_str());
  }
  if (*why) return luaL_error(L, "music(%d): %s", n, why);
  return 0;
}
// log(...): the arguments as tostring() gives them, spaces between, as one line of the host's log.
int f_log(lua_State* L) {
  if (S->logs > kLogLines) return 0;
  if (S->logs++ == kLogLines) {
    host_of(L).log("(more log lines this frame were dropped)");
    return 0;
  }
  const int n = lua_gettop(L);
  luaL_Buffer b;  // Lua's own buffer: a __tostring that errors unwinds nothing of ours
  luaL_buffinit(L, &b);
  for (int i = 1; i <= n; ++i) {
    if (i > 1) luaL_addchar(&b, ' ');
    luaL_tolstring(L, i, nullptr);
    luaL_addvalue(&b);
  }
  luaL_pushresult(&b);
  size_t len = 0;
  const char* line = lua_tolstring(L, -1, &len);
  host_of(L).log(std::string_view(line, std::min<size_t>(len, kLogLine)));
  return 0;
}

int f_wake(lua_State* L) {
  host_of(L).wake(lua_isnoneornil(L, 1) ? -1.0 : std::max(0.0, static_cast<double>(luaL_checknumber(L, 1))));
  return 0;
}
int f_level(lua_State* L) {
  host_of(L).level();
  return 0;
}
int f_time(lua_State* L) {
  lua_pushnumber(L, static_cast<lua_Number>((host_of(L).now_us() - S->t0_us) / 1e6));
  return 1;
}
// fps(): the rate now; fps(rate): that rate from the next frame; fps(active, idle): `active`
// while there's input and half a second after, `idle` otherwise. Rates divide 60.
int f_fps(lua_State* L) {
  if (lua_isnoneornil(L, 1)) {
    lua_pushinteger(L, S->cart->fps());
    return 1;
  }
  const lua_Integer active = luaL_checkinteger(L, 1), idle = luaL_optinteger(L, 2, 0);
  if (!Cart::rate_ok(static_cast<int>(active)) || (idle && !Cart::rate_ok(static_cast<int>(idle))))
    return luaL_error(L, "fps: a rate that divides 60 (1, 2, 3, 4, 5, 6, 10, 12, 15, 20, 30 or 60)");
  if (idle > active) return luaL_error(L, "fps: the idle rate is above the active one");
  S->cart->set_rate(static_cast<int>(active), static_cast<int>(idle));
  return 0;
}
int f_rnd(lua_State* L) {
  if (lua_istable(L, 1)) {
    const lua_Integer n = luaL_len(L, 1);
    if (n <= 0) {
      lua_pushnil(L);
      return 1;
    }
    lua_rawgeti(L, 1, 1 + static_cast<lua_Integer>(S->rnd01() * static_cast<float>(n)) % n);
    return 1;
  }
  const float x = lua_isnoneornil(L, 1) ? 1.0f : static_cast<float>(luaL_checknumber(L, 1));
  lua_pushnumber(L, S->rnd01() * x);
  return 1;
}
int f_srand(lua_State* L) {
  const float v = static_cast<float>(luaL_checknumber(L, 1));
  uint32_t seed;
  std::memcpy(&seed, &v, sizeof seed);
  S->rng = seed ? seed * 2654435761u : 0x9E3779B9u;
  return 0;
}
int f_flr(lua_State* L) {
  push_num(L, std::floor(static_cast<float>(luaL_optnumber(L, 1, 0))));
  return 1;
}
int f_ceil(lua_State* L) {
  push_num(L, std::ceil(static_cast<float>(luaL_optnumber(L, 1, 0))));
  return 1;
}
int f_abs(lua_State* L) {
  if (lua_isinteger(L, 1)) {
    const lua_Integer v = lua_tointeger(L, 1);
    lua_pushinteger(L, v < 0 ? -v : v);
  } else {
    lua_pushnumber(L, std::fabs(luaL_checknumber(L, 1)));
  }
  return 1;
}
int f_sgn(lua_State* L) {
  lua_pushinteger(L, luaL_checknumber(L, 1) < 0 ? -1 : 1);
  return 1;
}
int f_min(lua_State* L) {
  luaL_checknumber(L, 1);
  luaL_checknumber(L, 2);
  lua_pushvalue(L, lua_compare(L, 2, 1, LUA_OPLT) ? 2 : 1);
  return 1;
}
int f_max(lua_State* L) {
  luaL_checknumber(L, 1);
  luaL_checknumber(L, 2);
  lua_pushvalue(L, lua_compare(L, 1, 2, LUA_OPLT) ? 2 : 1);
  return 1;
}
// sin / cos in turns (1 = a full circle), PICO-8's way: sin is upside down (y grows down the
// screen). A 1024-entry table in flash, built at compile time (a Taylor series: C++17 has no
// constexpr std::sin), with linear interpolation: ~20 cycles against libm's sinf from flash (the
// trick from Jet's TrigLUT), within 5e-6 of the real thing. Nothing in RAM.
constexpr double taylor_sin(double x) {  // |x| <= pi
  double term = x, sum = x;
  for (int k = 1; k < 12; ++k) {
    term *= -x * x / ((2 * k) * (2 * k + 1));
    sum += term;
  }
  return sum;
}
struct SinTable {
  float v[1025];
  constexpr SinTable() : v{} {
    for (int i = 0; i <= 1024; ++i) {
      double x = i * (2 * 3.14159265358979323846 / 1024);
      if (x > 3.14159265358979323846) x -= 2 * 3.14159265358979323846;
      v[i] = static_cast<float>(taylor_sin(x));
    }
  }
};
constexpr SinTable kSinTable;
float turn_sin(float t) {
  const float f = (t - std::floor(t)) * 1024.0f;
  const int i = std::min(static_cast<int>(f), 1023);
  const float* v = kSinTable.v;
  return v[i] + (v[i + 1] - v[i]) * (f - static_cast<float>(i));
}
int f_sin(lua_State* L) {
  lua_pushnumber(L, -turn_sin(static_cast<float>(luaL_checknumber(L, 1))));
  return 1;
}
int f_cos(lua_State* L) {
  lua_pushnumber(L, turn_sin(static_cast<float>(luaL_checknumber(L, 1)) + 0.25f));
  return 1;
}

// trifill: a pixel is filled when its center is inside, with the top-left rule on the edges, in
// sixteenths of a pixel (integers, so check.py's preview is exact): solid 3D from a cart.
int f_trifill(lua_State* L) {
  auto sub = [&](int i) -> int64_t {  // a coordinate in 1/16 px, floored
    const lua_Number v = luaL_checknumber(L, i);
    if (!(v > -2048.0f)) return -2048 * 16;
    if (v > 2047.0f) return 2047 * 16;
    return static_cast<int64_t>(std::floor(v * 16.0f));
  };
  int64_t x[3] = {sub(1), sub(3), sub(5)}, y[3] = {sub(2), sub(4), sub(6)};
  const int c = color_arg(L, 7);
  for (int i = 0; i < 3; ++i) {
    x[i] -= static_cast<int64_t>(S->cam_x) * 16;
    y[i] -= static_cast<int64_t>(S->cam_y) * 16;
  }
  auto edge = [](int64_t ax, int64_t ay, int64_t bx, int64_t by, int64_t px, int64_t py) {
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
  };
  const int64_t area = edge(x[0], y[0], x[1], y[1], x[2], y[2]);
  if (area == 0) return 0;
  if (area < 0) {  // wind it one way: the inside is where every edge function is positive
    std::swap(x[1], x[2]);
    std::swap(y[1], y[2]);
  }
  // Each edge (a -> b): E(P) = dx (Py - ay) - dy (Px - ax); top or left edges keep E == 0.
  int64_t dx[3], dy[3], bias[3];
  for (int e = 0; e < 3; ++e) {
    const int a = e, b = (e + 1) % 3;
    dx[e] = x[b] - x[a];
    dy[e] = y[b] - y[a];
    bias[e] = (dy[e] < 0 || (dy[e] == 0 && dx[e] > 0)) ? 0 : -1;
  }
  // Pixels whose centers (16 px + 8) can be inside, within the clip.
  auto lo = [](int64_t v) { return static_cast<int>((v - 8 + 15 + (1 << 20) * 16) / 16 - (1 << 20)); };  // ceil((v - 8) / 16)
  auto hi = [](int64_t v) { return static_cast<int>((v - 8 + (1 << 20) * 16) / 16 - (1 << 20)); };       // floor((v - 8) / 16)
  const int x0 = std::max(lo(std::min({x[0], x[1], x[2]})), S->clip_x0), x1 = std::min(hi(std::max({x[0], x[1], x[2]})), S->clip_x1 - 1);
  const int y0 = std::max(lo(std::min({y[0], y[1], y[2]})), S->clip_y0), y1 = std::min(hi(std::max({y[0], y[1], y[2]})), S->clip_y1 - 1);
  if (x0 > x1) return 0;
  const uint8_t col = S->pmap[c];
  for (int py = y0; py <= y1; ++py) {
    const int64_t cy = py * 16 + 8, cx0 = x0 * 16 + 8;
    int64_t E[3];
    for (int e = 0; e < 3; ++e) E[e] = dx[e] * (cy - y[e]) - dy[e] * (cx0 - x[e]) + bias[e];
    // Along the row each E changes by -16 dy a pixel: the span is where all three are >= 0.
    int first = x0, last = x1;
    for (int e = 0; e < 3; ++e) {
      const int64_t step = -16 * dy[e];
      if (step == 0) {
        if (E[e] < 0) first = x1 + 1;
      } else if (step > 0) {  // rising: from the first pixel where E >= 0
        const int64_t need = E[e] >= 0 ? 0 : (-E[e] + step - 1) / step;
        first = std::max<int64_t>(first, x0 + need);
      } else {  // falling: up to the last pixel where E >= 0
        if (E[e] < 0) first = x1 + 1;
        else last = std::min<int64_t>(last, x0 + E[e] / -step);
      }
    }
    if (first <= last) std::memset(S->fb + py * kW + first, col, static_cast<size_t>(last - first + 1));
  }
  return 0;
}

int f_mid(lua_State* L) {  // check.py: a, b sorted, then max(a, min(b, c))
  lua_Number a = luaL_checknumber(L, 1), b = luaL_checknumber(L, 2);
  const lua_Number c = luaL_checknumber(L, 3);
  if (a > b) std::swap(a, b);
  push_num(L, std::max(a, std::min(b, c)));
  return 1;
}

// save / load: a small table as JSON (numbers, strings, booleans, nested tables).
void to_json(lua_State* L, int idx, hg::json::Value& out, int depth) {
  idx = lua_absindex(L, idx);
  if (depth > 8) luaL_error(L, "save: nested too deep");
  switch (lua_type(L, idx)) {
    case LUA_TNIL: out = nullptr; return;
    case LUA_TBOOLEAN: out = static_cast<bool>(lua_toboolean(L, idx)); return;
    case LUA_TNUMBER: out = static_cast<double>(lua_tonumber(L, idx)); return;
    case LUA_TSTRING: out = std::string(lua_tostring(L, idx)); return;
    case LUA_TTABLE: break;
    default: luaL_error(L, "save: can't keep a %s", luaL_typename(L, idx)); return;
  }
  // A sequence 1..n is an array; anything else an object with string keys.
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, idx));
  lua_Integer count = 0;
  bool seq = true;
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    ++count;
    if (!lua_isinteger(L, -2) || lua_tointeger(L, -2) < 1 || lua_tointeger(L, -2) > n) seq = false;
    lua_pop(L, 1);
  }
  if (seq && count == n && n > 0) {
    out = hg::json::Value::array();
    for (lua_Integer i = 1; i <= n; ++i) {
      lua_rawgeti(L, idx, i);
      hg::json::Value v;
      to_json(L, -1, v, depth + 1);
      out.push(std::move(v));
      lua_pop(L, 1);
    }
    return;
  }
  out = hg::json::Value::object();
  lua_pushnil(L);
  while (lua_next(L, idx)) {
    lua_pushvalue(L, -2);
    const char* k = luaL_tolstring(L, -1, nullptr);
    hg::json::Value v;
    to_json(L, -3, v, depth + 1);
    out.set(k, std::move(v));
    lua_pop(L, 3);  // the key's string, its copy, the value
  }
}

void push_json(lua_State* L, const hg::json::Value& v) {
  luaL_checkstack(L, 4, "data nested too deep");
  switch (v.type()) {
    case hg::json::Type::Null: lua_pushnil(L); break;
    case hg::json::Type::Bool: lua_pushboolean(L, v.as_bool()); break;
    case hg::json::Type::Number: {
      // Whole numbers exactly, from the double: a saved timestamp (~1.8e9) is past a float's 24 bits.
      const double d = v.as_number();
      if (d == std::floor(d) && d >= static_cast<double>(LUA_MININTEGER) && d <= static_cast<double>(LUA_MAXINTEGER))
        lua_pushinteger(L, static_cast<lua_Integer>(d));
      else
        push_num(L, static_cast<float>(d));
      break;
    }
    case hg::json::Type::String: lua_pushlstring(L, v.as_string().data(), v.as_string().size()); break;
    case hg::json::Type::Array: {
      lua_createtable(L, static_cast<int>(v.elements().size()), 0);
      lua_Integer i = 1;
      for (const auto& e : v.elements()) {
        push_json(L, e);
        lua_rawseti(L, -2, i++);
      }
      break;
    }
    case hg::json::Type::Object:
      lua_createtable(L, 0, static_cast<int>(v.members().size()));
      for (const auto& [k, e] : v.members()) {
        push_json(L, e);
        lua_setfield(L, -2, k.c_str());
      }
      break;
  }
}

int f_save(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  hg::json::Value v;
  to_json(L, 1, v, 0);
  const std::string s = v.dump();
  if (s.size() > kMaxSave) return luaL_error(L, "save: %d bytes, the limit is %d", static_cast<int>(s.size()), static_cast<int>(kMaxSave));
  if (!host_of(L).store_save(S->cart->name(), s)) return luaL_error(L, "save: the board couldn't keep it");
  return 0;
}
int f_load(lua_State* L) {
  const std::string s = host_of(L).load_save(S->cart->name());
  hg::json::Value v;
  if (s.empty() || !hg::json::parse(s, v)) {
    lua_pushnil(L);
    return 1;
  }
  push_json(L, v);
  return 1;
}

// now(): seconds since 1970 (UTC), or nil while the time is unknown (no RTC, no network yet).
// An integer: exact on the board's 32-bit numbers until 2038.
int f_now(lua_State* L) {
  const std::time_t t = static_cast<std::time_t>(S->host->unix_time());
  if (t < kTimeKnownAfter) lua_pushnil(L);
  else lua_pushinteger(L, static_cast<lua_Integer>(t));
  return 1;
}

// date([t]): local time (the host's timezone) as os.date("*t") gives it: year, month, day, hour,
// min, sec, wday (1 = Sunday), yday; for now() when t is left out. nil while the time is unknown.
int f_date(lua_State* L) {
  const std::time_t t = lua_isnoneornil(L, 1) ? static_cast<std::time_t>(S->host->unix_time()) : static_cast<std::time_t>(luaL_checkinteger(L, 1));
  if (t < kTimeKnownAfter) {
    lua_pushnil(L);
    return 1;
  }
  std::tm tm{};
  localtime_r(&t, &tm);
  lua_createtable(L, 0, 8);
  const std::pair<const char*, int> fields[] = {{"year", tm.tm_year + 1900}, {"month", tm.tm_mon + 1}, {"day", tm.tm_mday},
                                               {"hour", tm.tm_hour},         {"min", tm.tm_min},        {"sec", tm.tm_sec},
                                               {"wday", tm.tm_wday + 1},     {"yday", tm.tm_yday + 1}};
  for (const auto& [k, v] : fields) {
    lua_pushinteger(L, v);
    lua_setfield(L, -2, k);
  }
  return 1;
}

// data(name): the channel as a table, made again only when the channel changed.
int f_data(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  uint32_t ver = 0;
  const hg::json::Value* v = host_of(L).data(name, &ver);
  if (!v || v->is_null()) {
    lua_pushnil(L);
    return 1;
  }
  lua_getfield(L, LUA_REGISTRYINDEX, "hg.data");  // name -> {ver, value}
  if (lua_getfield(L, -1, name) == LUA_TTABLE) {
    lua_rawgeti(L, -1, 1);
    const bool same = static_cast<uint32_t>(lua_tointeger(L, -1)) == ver;
    lua_pop(L, 1);
    if (same) {
      lua_rawgeti(L, -1, 2);
      return 1;
    }
  }
  lua_pop(L, 1);
  lua_createtable(L, 2, 0);
  lua_pushinteger(L, static_cast<lua_Integer>(ver));
  lua_rawseti(L, -2, 1);
  push_json(L, *v);
  lua_pushvalue(L, -1);
  lua_rawseti(L, -3, 2);  // entry[2] = value
  lua_insert(L, -2);      // value under entry
  lua_setfield(L, -3, name);
  return 1;
}
int f_act(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  const char* arg = luaL_optstring(L, 2, "");
  if (!host_of(L).accepts_act(name)) return luaL_error(L, "act: unknown command '%s'", name);
  host_of(L).act(name, arg);
  return 0;
}
int f_not_yet(lua_State* L) {
  return luaL_error(L, "%s: not on the board yet", lua_tostring(L, lua_upvalueindex(1)));
}
// ---- 3D (the stage) ----

// ---- hd: the full-color layer ----


Hd& hd_of(lua_State* L) {
  if (!S->hd) {
    const int w = S->full ? S->caps.glass_w : Hd::kW, h = S->full ? S->caps.glass_h : Hd::kH;
    const size_t n = static_cast<size_t>(Hd::pitch_of(w)) * h;
    const size_t sizes[4] = {n * 2, n, n * 2, n};
    for (int i = 0; i < 4; ++i) {
      S->hd_mem[i] = S->host->alloc(sizes[i]);
      if (!S->hd_mem[i]) luaL_error(L, "hd: no memory for the layer (%d KB)", static_cast<int>(n * 6 / 1024));
    }
    S->hd = std::make_unique<Hd>(w, h, static_cast<uint16_t*>(S->hd_mem[0]), static_cast<uint8_t*>(S->hd_mem[1]),
                                 static_cast<uint16_t*>(S->hd_mem[2]), static_cast<uint8_t*>(S->hd_mem[3]));
  }
  return *S->hd;
}

uint32_t hd_rgb(lua_State* L, int i) {
  const lua_Integer v = luaL_checkinteger(L, i);
  if (v < 0 || v > 0xFFFFFF) luaL_error(L, "%s isn't a 0xRRGGBB color", hex_of(v).c_str());
  return static_cast<uint32_t>(v);
}
uint8_t hd_alpha(lua_State* L, int i) {
  const lua_Integer v = luaL_optinteger(L, i, 255);
  return static_cast<uint8_t>(v < 0 ? 0 : (v > 255 ? 255 : v));
}
// Within +-32767, as coord() keeps the picture's (NaN reads as the low end): the layer's maths stays in range.
float hd_clamp(lua_Number v) { return !(v > -32767) ? -32767.0f : (v > 32767 ? 32767.0f : static_cast<float>(v)); }
float hd_num(lua_State* L, int i) { return hd_clamp(luaL_checknumber(L, i)); }
float hd_opt(lua_State* L, int i, float d) { return hd_clamp(luaL_optnumber(L, i, d)); }
Hd::Font hd_font(lua_State* L, int i) {
  const char* f = luaL_optstring(L, i, "mono");
  if (!std::strcmp(f, "mono")) return Hd::kMono;
  if (!std::strcmp(f, "bold")) return Hd::kBold;
  if (!std::strcmp(f, "vt")) return Hd::kVt;
  luaL_error(L, "hd: no font '%s' (mono, bold, vt)", f);
  return Hd::kMono;
}

int hd_clear(lua_State* L) {
  hd_of(L).clear(lua_isnoneornil(L, 1) ? 0 : hd_rgb(L, 1), static_cast<uint8_t>(luaL_optinteger(L, 2, lua_isnoneornil(L, 1) ? 0 : 255)));
  return 0;
}
int hd_order(lua_State* L) {
  const char* o = luaL_checkstring(L, 1);
  if (std::strcmp(o, "over") && std::strcmp(o, "under")) return luaL_error(L, "hd.order: \"over\" or \"under\", not '%s'", o);
  hd_of(L).set_over(!std::strcmp(o, "over"));
  return 0;
}
int hd_scanlines(lua_State* L) {  // hd.scanlines(on), or hd.scanlines(): whether they're on
  if (lua_isnoneornil(L, 1)) {
    lua_pushboolean(L, hd_of(L).scanlines());
    return 1;
  }
  hd_of(L).set_scanlines(lua_toboolean(L, 1));
  return 0;
}
int hd_rect(lua_State* L) {
  hd_of(L).rect(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_num(L, 4), hd_rgb(L, 5), hd_alpha(L, 6), hd_opt(L, 7, 0));
  return 0;
}
int hd_frame(lua_State* L) {
  hd_of(L).frame(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_num(L, 4), hd_rgb(L, 5), hd_opt(L, 6, 1), hd_opt(L, 7, 0), hd_alpha(L, 8));
  return 0;
}
int hd_circle(lua_State* L) {
  hd_of(L).circle(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_rgb(L, 4), hd_alpha(L, 5));
  return 0;
}
int hd_ring(lua_State* L) {
  hd_of(L).ring(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_rgb(L, 4), hd_opt(L, 5, 1), hd_alpha(L, 6));
  return 0;
}
int hd_line(lua_State* L) {
  hd_of(L).line(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_num(L, 4), hd_rgb(L, 5), hd_opt(L, 6, 1), hd_alpha(L, 7));
  return 0;
}
int hd_gradient(lua_State* L) {
  hd_of(L).gradient(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_num(L, 4), hd_rgb(L, 5), hd_rgb(L, 6), hd_alpha(L, 7));
  return 0;
}
int hd_text(lua_State* L) {
  size_t len = 0;
  const char* str = luaL_tolstring(L, 1, &len);  // numbers print as text too
  const std::string text(str, len);
  lua_pop(L, 1);
  const char* al = luaL_optstring(L, 7, "left");
  if (std::strcmp(al, "left") && std::strcmp(al, "center") && std::strcmp(al, "right"))
    return luaL_error(L, "hd.text: align is \"left\", \"center\" or \"right\", not '%s'", al);
  const Hd::Align align = !std::strcmp(al, "center") ? Hd::kCenter : !std::strcmp(al, "right") ? Hd::kRight : Hd::kLeft;
  const float w = hd_of(L).text(text, hd_num(L, 2), hd_num(L, 3), hd_rgb(L, 4), static_cast<int>(luaL_optinteger(L, 5, 16)),
                                hd_font(L, 6), align, hd_alpha(L, 8));
  lua_pushnumber(L, w);
  return 1;
}
int hd_measure(lua_State* L) {
  size_t len = 0;
  const char* str = luaL_tolstring(L, 1, &len);
  const std::string text(str, len);
  lua_pop(L, 1);
  float h = 0;
  const float w = hd_of(L).measure(text, static_cast<int>(luaL_optinteger(L, 2, 16)), hd_font(L, 3), &h);
  lua_pushnumber(L, w);
  lua_pushnumber(L, h);
  return 2;
}
int hd_wrap(lua_State* L) {
  size_t len = 0;
  const char* str = luaL_checklstring(L, 1, &len);
  const auto lines = hd_of(L).wrap(std::string_view(str, len), hd_num(L, 2), static_cast<int>(luaL_optinteger(L, 3, 16)), hd_font(L, 4));
  lua_createtable(L, static_cast<int>(lines.size()), 0);
  for (size_t i = 0; i < lines.size(); ++i) {
    lua_pushlstring(L, lines[i].data(), lines[i].size());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}
// hd.inset(y, [h]): how far in from each side rows [y, y + h) show on this glass (its corners are
// rounded: caps().corner); 0 for the square window, or rows clear of the corners.
int hd_inset(lua_State* L) {
  const float y = hd_num(L, 1), h = std::max(1.0f, hd_opt(L, 2, 1));
  const float R = S->full ? static_cast<float>(S->caps.corner) : 0, H = static_cast<float>(S->caps.glass_h);
  const float d = std::min(y + 0.5f, H - (y + h - 1) - 0.5f);  // the band's row nearest the top or bottom edge
  float inset = 0;
  if (R > 0 && d < R) inset = R - std::sqrt(std::max(0.0f, R * R - (R - std::max(0.0f, d)) * (R - std::max(0.0f, d))));
  lua_pushinteger(L, static_cast<lua_Integer>(std::ceil(inset)));
  return 1;
}
int hd_scroll(lua_State* L) {
  hd_of(L).scroll(static_cast<int>(hd_num(L, 1)), static_cast<int>(hd_num(L, 2)), static_cast<int>(hd_num(L, 3)),
                  static_cast<int>(hd_num(L, 4)), static_cast<int>(hd_num(L, 5)));
  return 0;
}
int hd_clip(lua_State* L) {
  if (lua_isnoneornil(L, 1)) hd_of(L).unclip();
  else hd_of(L).clip(hd_num(L, 1), hd_num(L, 2), hd_num(L, 3), hd_num(L, 4));
  return 0;
}
// hd.image(name, x, y, [w, h, a]): true when drawn; false when the cart has no such image (or it
// hasn't arrived: a cart can show a placeholder).
int hd_image(lua_State* L) {
  const char* name = luaL_checkstring(L, 1);
  const float x = hd_num(L, 2), y = hd_num(L, 3), w = hd_opt(L, 4, 0), h = hd_opt(L, 5, 0);
  const uint8_t a = hd_alpha(L, 6);
  Hd& hd = hd_of(L);
  const HdImage* img = S->image(name);
  if (img) hd.image(*img, x, y, w, h, a);
  lua_pushboolean(L, img != nullptr);
  return 1;
}
int hd_images(lua_State* L) {
  const auto names = S->host->image_names();
  lua_createtable(L, static_cast<int>(names.size()), 0);
  for (size_t i = 0; i < names.size(); ++i) {
    lua_pushlstring(L, names[i].data(), names[i].size());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

Stage& stage_of(lua_State*) {
  if (!S->stage) S->stage = std::make_unique<Stage>(1, kStageW, kStageX + Stage::kW / 2.0f);
  return *S->stage;
}
uint32_t rgb_arg(lua_State* L, int i, const char* fn) {
  const lua_Integer v = luaL_checkinteger(L, i);
  if (v < 0 || v > 0xFFFFFF) luaL_error(L, "%s: colors are 0xRRGGBB", fn);
  return static_cast<uint32_t>(v);
}
float num(lua_State* L, int i) { return static_cast<float>(luaL_checknumber(L, i)); }
float opt_num(lua_State* L, int i, float d) { return static_cast<float>(luaL_optnumber(L, i, d)); }
int id_or_error(lua_State* L, int id, const char* fn) {
  if (id < 0) return luaL_error(L, "%s: %s", fn, stage_of(L).error().c_str());
  lua_pushinteger(L, id + 1);  // ids from 1
  return 1;
}
int obj_arg(lua_State* L, int i) { return static_cast<int>(luaL_checkinteger(L, i)) - 1; }
int ok_or_error(lua_State* L, bool ok, const char* fn) {
  if (!ok) return luaL_error(L, "%s: %s", fn, stage_of(L).error().c_str());
  return 0;
}
// A Lua array of numbers into `out`.
void numbers(lua_State* L, int i, std::vector<float>& out, const char* what) {
  luaL_checktype(L, i, LUA_TTABLE);
  const lua_Integer n = luaL_len(L, i);
  if (n > 3 * Stage::kMaxVertices) luaL_error(L, "shape: %s has %d numbers", what, static_cast<int>(n));
  out.resize(static_cast<size_t>(n));
  for (lua_Integer k = 1; k <= n; ++k) {
    lua_rawgeti(L, i, k);
    int isnum = 0;
    out[static_cast<size_t>(k - 1)] = static_cast<float>(lua_tonumberx(L, -1, &isnum));
    lua_pop(L, 1);
    if (!isnum) luaL_error(L, "shape: %s[%d] isn't a number", what, static_cast<int>(k));
  }
}
int f_shape(lua_State* L) {
  Stage& st = stage_of(L);
  if (lua_istable(L, 1)) {  // shape(verts, faces[, smooth[, uvs]])
    std::vector<float> xyz, f, uv;
    numbers(L, 1, xyz, "the vertices");
    numbers(L, 2, f, "the faces");
    std::vector<int> idx(f.size());
    for (size_t k = 0; k < f.size(); ++k) idx[k] = static_cast<int>(f[k]) - 1;  // faces count from 1
    if (lua_istable(L, 4)) numbers(L, 4, uv, "the texture coordinates");
    return id_or_error(L, st.mesh(xyz, idx, lua_toboolean(L, 3), lua_istable(L, 4) ? &uv : nullptr), "shape");
  }
  const char* kind = luaL_checkstring(L, 1);
  const auto segs = [&](int i, int d) { return static_cast<int>(luaL_optinteger(L, i, d)); };
  int id;
  if (!std::strcmp(kind, "box")) id = st.box(num(L, 2), opt_num(L, 3, num(L, 2)), opt_num(L, 4, num(L, 2)));
  else if (!std::strcmp(kind, "sphere")) id = st.sphere(num(L, 2), segs(3, 16));
  else if (!std::strcmp(kind, "torus")) id = st.torus(num(L, 2), num(L, 3), segs(4, 24));
  else if (!std::strcmp(kind, "cylinder")) id = st.cylinder(num(L, 2), num(L, 3), segs(4, 16));
  else if (!std::strcmp(kind, "cone")) id = st.cone(num(L, 2), num(L, 3), segs(4, 16));
  else return luaL_error(L, "shape: no shape '%s' (box, sphere, torus, cylinder, cone, or vertices and faces)", kind);
  return id_or_error(L, id, "shape");
}
int f_obj(lua_State* L) {
  Stage& st = stage_of(L);
  const int shape = static_cast<int>(luaL_checkinteger(L, 1)) - 1;
  return id_or_error(L, st.add(shape, rgb_arg(L, 2, "obj"), static_cast<int>(luaL_optinteger(L, 3, 0)), lua_toboolean(L, 4)), "obj");
}
int f_paint(lua_State* L) {
  return ok_or_error(L, stage_of(L).recolor(obj_arg(L, 1), rgb_arg(L, 2, "paint"), static_cast<int>(luaL_optinteger(L, 3, 0)), lua_toboolean(L, 4)),
                     "paint");
}
// texture(w, h, texels): w x h color numbers (hex digits, as SPRITES), in the palette as it is now
int f_texture(lua_State* L) {
  Stage& st = stage_of(L);
  const int w = static_cast<int>(luaL_checkinteger(L, 1)), h = static_cast<int>(luaL_checkinteger(L, 2));
  size_t n = 0;
  const char* s = luaL_checklstring(L, 3, &n);
  std::vector<uint8_t> texels;
  texels.reserve(n);
  for (size_t k = 0; k < n; ++k) {
    const char ch = s[k];
    if (ch == ' ' || ch == '\n' || ch == '\t' || ch == '\r') continue;  // (laid out in rows, if the cart likes)
    const int v = ch >= '0' && ch <= '9' ? ch - '0' : ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 : ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
    if (v < 0) return luaL_error(L, "texture: '%c' isn't a color number (0-9, a-f)", ch);
    texels.push_back(static_cast<uint8_t>(v));
  }
  uint32_t pal[16];
  for (int k = 0; k < 16; ++k) pal[k] = S->cart->colors()[k];
  return id_or_error(L, st.texture(w, h, texels, pal), "texture");
}
// skin(o, tex[, repeat_u, repeat_v]): object o in a texture (nil: its color again)
int f_skin(lua_State* L) {
  const int tex = lua_isnoneornil(L, 2) ? -1 : static_cast<int>(luaL_checkinteger(L, 2)) - 1;
  if (!lua_isnoneornil(L, 2) && tex < 0) return luaL_error(L, "skin: no texture %d", tex + 1);
  return ok_or_error(L, stage_of(L).skin(obj_arg(L, 1), tex, opt_num(L, 3, 1), opt_num(L, 4, opt_num(L, 3, 1))), "skin");
}
int f_place(lua_State* L) {
  return ok_or_error(L, stage_of(L).place(obj_arg(L, 1), num(L, 2), num(L, 3), num(L, 4), opt_num(L, 5, 0), opt_num(L, 6, 0), opt_num(L, 7, 0)),
                     "place");
}
int f_orient(lua_State* L) {
  float m[9];
  if (lua_istable(L, 2)) {
    for (int k = 0; k < 9; ++k) {
      lua_rawgeti(L, 2, k + 1);
      m[k] = static_cast<float>(luaL_checknumber(L, -1));
      lua_pop(L, 1);
    }
  } else {
    for (int k = 0; k < 9; ++k) m[k] = num(L, k + 2);
  }
  return ok_or_error(L, stage_of(L).orient(obj_arg(L, 1), m), "orient");
}
int f_hide(lua_State* L) { return ok_or_error(L, stage_of(L).hide(obj_arg(L, 1), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)), "hide"); }
int f_view(lua_State* L) {
  stage_of(L).view(num(L, 1), num(L, 2), num(L, 3), opt_num(L, 4, 0), opt_num(L, 5, 0), opt_num(L, 6, 60));
  return 0;
}
int f_light(lua_State* L) {
  stage_of(L).light(num(L, 1), num(L, 2), static_cast<int>(luaL_optinteger(L, 3, 255)), lua_isnoneornil(L, 4) ? 0x22283E : rgb_arg(L, 4, "light"));
  return 0;
}
int f_sky(lua_State* L) {
  const uint32_t top = rgb_arg(L, 1, "sky");
  stage_of(L).sky(top, lua_isnoneornil(L, 2) ? top : rgb_arg(L, 2, "sky"));
  return 0;
}
int f_ground(lua_State* L) {
  stage_of(L).ground(num(L, 1), lua_isnoneornil(L, 2) ? 0x000000 : rgb_arg(L, 2, "ground"));
  return 0;
}
int f_shadow(lua_State* L) { return ok_or_error(L, stage_of(L).shadow(obj_arg(L, 1), lua_isnoneornil(L, 2) || lua_toboolean(L, 2)), "shadow"); }
int f_project(lua_State* L) {
  float sx, sy, d;
  if (!stage_of(L).project(num(L, 1), num(L, 2), num(L, 3), &sx, &sy, &d)) return 0;
  constexpr float k = static_cast<float>(kW) / Stage::kW;  // the picture's coordinates
  lua_pushnumber(L, (sx - kStageX) * k);
  lua_pushnumber(L, sy * k);
  lua_pushnumber(L, d);
  return 3;
}

// res([mode]): the scene drawn at full size ("full"), half ("half": each pixel 2x2, a quarter of the
// drawing), or "auto" (the board halves it while frames run over, full again when there's room).
// Returns the size it's drawn at now.
int f_res(lua_State* L) {
  Stage& st = stage_of(L);
  if (!lua_isnoneornil(L, 1)) {
    const std::string m = luaL_checkstring(L, 1);
    if (m == "full" || m == "half") {
      st.set_auto_res(false);
      st.set_scale(m == "half" ? 2 : 1);
    } else if (m == "auto") {
      st.set_auto_res(true);
    } else {
      return luaL_error(L, "res: \"full\", \"half\" or \"auto\" (not \"%s\")", m.c_str());
    }
  }
  lua_pushstring(L, st.wanted_scale() == 2 ? "half" : "full");
  return 1;
}

int f_stat(lua_State* L) {
  switch (coord(L, 1)) {
    case 0: lua_pushnumber(L, static_cast<lua_Number>(S->heap.used / 1024.0)); break;  // memory, KB
    case 1: lua_pushnumber(L, static_cast<lua_Number>(S->cart->last().lua_us / S->cart->period_us())); break;  // CPU, of a frame
    case 7: lua_pushnumber(L, S->cart->real_fps()); break;
    default: lua_pushinteger(L, 0); break;
  }
  return 1;
}

// Once a frame is stopped (the deadline, the budget), every instruction raises the same error
// again, so a cart's own pcall can catch it but not carry on: `while true do pcall(f) end` would
// otherwise catch each one and never leave the frame. Cart::run clears the hook after the frame.
void tripped_hook(lua_State* L, lua_Debug*) {
  lua_pushstring(L, S->trip_msg.c_str());
  lua_error(L);
}
// Stops the frame with the message on top of the stack.
[[noreturn]] void trip(lua_State* L) {
  S->trip_msg = lua_tostring(L, -1);
  lua_sethook(L, tripped_hook, LUA_MASKCOUNT, 1);
  lua_error(L);
  __builtin_unreachable();
}

// Set by Cart::interrupt() when the deadline passes: the next instruction lands here.
void deadline_hook(lua_State* L, lua_Debug*) {
  lua_pushfstring(L, "ran over %d ms (an endless loop?)", static_cast<int>((S->limit_us + 999) / 1000));
  trip(L);
}

// A host that polls (Host::deadline_by_polling): every kPollEvery instructions, the clock.
constexpr int kPollEvery = 20000;
void poll_hook(lua_State* L, lua_Debug*) {
  if (S->host->now_us() - S->frame_t0 > S->limit_us) {
    luaL_where(L, 1);  // as luaL_error said it (in a hook, level 1 is no one: no position)
    lua_pushfstring(L, "ran over %d ms (an endless loop?)", static_cast<int>((S->limit_us + 999) / 1000));
    lua_concat(L, 2);
    trip(L);
  }
}

// A host that counts instructions (Host::frame_budget): every kCountEvery, as check.py does; past
// the budget, the cart stops in check.py's words. A host that polls the clock gets that here too.
// check.py runs each frame inside a little Lua function of its own (about 15 instructions more than
// here), and both count in 100s, so this stops a count earlier: a frame it lets through is under
// 30,000 instructions, which check.py counts as at most 30,000 too.
constexpr int kCountEvery = 100;
void count_hook(lua_State* L, lua_Debug* ar) {
  S->ops += kCountEvery;
  if (S->budget && S->ops > S->budget - kCountEvery) {
    luaL_where(L, 0);  // the cart's line it was on ("cart:4: "): in a hook, luaL_error's level 1 is no one
    lua_pushfstring(L, "over budget: more than %d Lua instructions in one frame", S->budget);
    lua_concat(L, 2);
    trip(L);
  }
  if (S->host->deadline_by_polling() && ++S->polls >= kPollEvery / kCountEvery) {
    S->polls = 0;
    poll_hook(L, ar);
  }
}

int traceback(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  if (!msg) msg = lua_pushfstring(L, "(error object is a %s value)", luaL_typename(L, 1));
  luaL_traceback(L, L, msg, 1);
  return 1;
}


// ---- reloading: new code into a running cart, its data kept (Cart::reload) -----------------------
//
// The Lua side (kReloadLua) decides what to keep; these give it what a cart never gets: upvalues
// (a cart's top-level locals live in its functions' upvalues) and which functions a chunk's top level made.
const Proto* proto_of(lua_State* L, int i) {
  const TValue* o = arg(L, i);
  return ttisLclosure(o) ? clLvalue(o)->p : nullptr;
}
int h_up(lua_State* L) {  // up(f, i): the upvalue's name and value, or nothing
  const char* name = lua_getupvalue(L, 1, static_cast<int>(luaL_checkinteger(L, 2)));
  if (!name) return 0;
  lua_pushstring(L, name);
  lua_insert(L, -2);
  return 2;
}
int h_setup(lua_State* L) {  // setup(f, i, v)
  lua_settop(L, 3);
  lua_setupvalue(L, 1, static_cast<int>(luaL_checkinteger(L, 2)));
  return 0;
}
int h_id(lua_State* L) {  // id(f, i): the upvalue's cell (two functions share a local when they share it)
  lua_pushlightuserdata(L, lua_upvalueid(L, 1, static_cast<int>(luaL_checkinteger(L, 2))));
  return 1;
}
int h_join(lua_State* L) {  // join(f1, i1, f2, i2): f1's upvalue i1 becomes f2's i2
  lua_upvaluejoin(L, 1, static_cast<int>(luaL_checkinteger(L, 2)), 3, static_cast<int>(luaL_checkinteger(L, 4)));
  return 0;
}
int h_top(lua_State* L) {  // top(f, main): f was made by main's top level (its prototype is one of main's)
  const Proto *p = proto_of(L, 1), *m = proto_of(L, 2);
  bool top = false;
  for (int i = 0; p && m && i < m->sizep && !top; ++i) top = m->p[i] == p;
  lua_pushboolean(L, top);
  return 1;
}
int h_getmt(lua_State* L) {  // the metatable, whatever its __metatable says
  if (!lua_getmetatable(L, 1)) lua_pushnil(L);
  return 1;
}

// What stays and what changes when new code comes to a running cart. At the start (begin, after
// the cart's top level) it notes what the code set up: each global the cart made and each
// top-level local, copied. At a reload the new top level runs into a table of its own, then each
// of its values is merged into the running one: functions always the new code's; any other value
// the new one where the game left it as the old code set it up, the game's where it changed it;
// tables patched in place (so whatever holds one, a scene, an object's metatable, sees the new
// code), key by key by the same rule. New top-level locals take over the old ones' cells.
constexpr char kReloadLua[] = R"lua(
local H, G = ...
local next, type, rawget, rawset, rawequal, setmetatable = next, type, rawget, rawset, rawequal, setmetatable
local up, setup, id, join, top, getmt = H.up, H.setup, H.id, H.join, H.top, H.getmt
local builtin, init_g, init_up, init_mt, main = {}, {}, {}, setmetatable({}, {__mode = "k"}), nil

local function copy(v, memo)
  if type(v) ~= "table" then return v end
  local c = memo[v]
  if c then return c end
  c = {}
  memo[v] = c
  init_mt[c] = getmt(v)
  for k, x in next, v do rawset(c, k, copy(x, memo)) end
  return c
end

local function same(a, b, memo)
  if rawequal(a, b) then return true end
  if type(a) ~= "table" or type(b) ~= "table" then return false end
  memo = memo or {}
  if memo[a] == b then return true end
  memo[a] = b
  for k, x in next, a do if not same(x, rawget(b, k), memo) then return false end end
  for k in next, b do if rawget(a, k) == nil then return false end end
  return true
end

local function merge(old, init, new, seen)
  if type(new) == "table" and type(old) == "table" then
    if rawequal(old, new) or seen[old] then return old end
    seen[old] = true
    local it = type(init) == "table" and init or nil
    for k, nv in next, new do rawset(old, k, merge(rawget(old, k), it and rawget(it, k), nv, seen)) end
    if it then
      for k, ov in next, old do
        if rawget(new, k) == nil and rawget(it, k) ~= nil and same(ov, rawget(it, k)) then rawset(old, k, nil) end
      end
    end
    local om, nm = getmt(old), getmt(new)
    if om == nil and nm ~= nil then setmetatable(old, nm)
    elseif type(om) == "table" and type(nm) == "table" then merge(om, it and init_mt[it], nm, seen) end
    return old
  end
  if type(new) == "function" then
    if old == nil or rawequal(old, init) then return new end
    return old
  end
  if (old == nil and init == nil) or same(old, init) then return new end
  return old
end

-- Every function `m`'s top level made that can be reached from `roots` (through tables and other
-- functions' upvalues), each top-level local they use: name -> {f, i, cell, uses = {{f, i}...}}.
-- A name two locals share is false (no telling them apart).
local function locals(roots, m)
  local out, seen, stack = {}, {}, {}
  for _, r in next, roots do stack[#stack + 1] = r end
  while #stack > 0 do
    local v = stack[#stack]
    stack[#stack] = nil
    if (type(v) == "table" or type(v) == "function") and not seen[v] then
      seen[v] = true
      if type(v) == "table" then
        for k, x in next, v do stack[#stack + 1] = k; stack[#stack + 1] = x end
        local mt = getmt(v)
        if mt then stack[#stack + 1] = mt end
      else
        local is_top = top(v, m)
        local i = 1
        while true do
          local name, x = up(v, i)
          if not name then break end
          stack[#stack + 1] = x
          if is_top and name ~= "_ENV" and name ~= "" then
            local cell, l = id(v, i), out[name]
            if l == nil then
              out[name] = {f = v, i = i, cell = cell, value = x, uses = {{v, i}}}
            elseif l and l.cell == cell then
              l.uses[#l.uses + 1] = {v, i}
            else
              out[name] = false
            end
          end
          i = i + 1
        end
      end
    end
  end
  return out
end

local R = {}
function R.before()  -- before the cart's top level: what's there is the runtime's
  for k, v in next, G do builtin[k] = v end
end
function R.begin(m)  -- after it: what the code set up
  main = m
  local memo = {}
  for k, v in next, G do if not rawequal(builtin[k], v) then init_g[k] = copy(v, memo) end end
  init_up = {}
  for name, l in next, locals({G}, m) do
    if l then init_up[name] = {f = l.f, i = l.i, init = copy(l.value, memo)} end
  end
end
function R.reload(m, P)
  local memo = {}
  local new_g = {}
  for k, v in next, P do new_g[k] = copy(v, memo) end
  local roots = {P}
  for k, v in next, P do roots[#roots + 1] = v end
  local new_l = locals(roots, m)
  local new_up = {}
  for name, l in next, new_l do if l then new_up[name] = {f = l.f, i = l.i, init = copy(l.value, memo)} end end
  local seen = {}
  for k, v in next, P do rawset(G, k, merge(rawget(G, k), init_g[k], v, seen)) end
  for k, v in next, init_g do
    if rawget(P, k) == nil and same(rawget(G, k), v) then rawset(G, k, nil) end
  end
  for name, l in next, new_l do
    local o = l and init_up[name]
    if o then
      local _, now = up(o.f, o.i)
      setup(o.f, o.i, merge(now, o.init, l.value, seen))
      for _, u in next, l.uses do join(u[1], u[2], o.f, o.i) end
      new_up[name] = {f = o.f, i = o.i, init = new_up[name].init}
    end
  end
  setup(m, 1, G)  -- the new code's globals are the cart's (its top level ran into P)
  init_g, init_up, main = new_g, new_up, m
end
return R
)lua";

}  // namespace

// ---- Cart ------------------------------------------------------------------------

Cart::Cart(Host& host) : host_(host) {}

Cart::~Cart() {
  if (S == impl_) S = nullptr;
  if (L_) lua_close(L_);
  if (impl_) {
    host_.sound(nullptr);  // its sounds and song end with it
    host_.music(nullptr);
    impl_->heap.close();
    if (fb_) host_.release_fast(fb_);
    if (shown_) host_.release_fast(shown_);
    impl_->drop_images();
    impl_->hd.reset();
    for (void* m : impl_->hd_mem)
      if (m) host_.release(m);
    delete impl_;
  }
}

void Cart::set_shift(int y, int dx) {
  dx = std::clamp(dx, -kW, kW);
  if (shift_[y] == dx) return;
  shift_[y] = static_cast<int16_t>(dx);
  if (dx && !shown_) {
    shown_ = static_cast<uint8_t*>(host_.alloc_fast(kW * kH));
    if (!shown_) luaL_error(L_, "rowshift: no memory for the moved picture");
  }
  shifted_ = false;
  for (int r = 0; r < kH && !shifted_; ++r) shifted_ = shift_[r] != 0;
  // (hosts read picture() between frames: make_shown() builds it when this one ends)
}
void Cart::make_shown() {
  if (!shifted_) return;
  for (int y = 0; y < kH; ++y) {
    const uint8_t* src = fb_ + y * kW;
    uint8_t* d = shown_ + y * kW;
    const int dx = shift_[y];
    if (dx == 0) {
      std::memcpy(d, src, kW);
    } else if (dx > 0) {
      std::memset(d, src[0], static_cast<size_t>(dx));
      std::memcpy(d + dx, src, static_cast<size_t>(kW - dx));
    } else {
      std::memcpy(d, src - dx, static_cast<size_t>(kW + dx));
      std::memset(d + kW + dx, src[kW - 1], static_cast<size_t>(-dx));
    }
  }
}
void Cart::set_row_color(int y, int c, int64_t rgb) {
  const uint16_t bit = static_cast<uint16_t>(1u << c);
  if (rgb < 0) {
    if (!(row_mask_[y] & bit)) return;
    row_mask_[y] = static_cast<uint16_t>(row_mask_[y] & ~bit);
  } else {
    if ((row_mask_[y] & bit) && row_rgb_[y][c] == static_cast<uint32_t>(rgb)) return;
    row_mask_[y] = static_cast<uint16_t>(row_mask_[y] | bit);
    row_rgb_[y][c] = static_cast<uint32_t>(rgb);
  }
  ++rows_ver_;
}

size_t Cart::memory() const { return impl_ ? impl_->heap.used : 0; }
Stage* Cart::stage() const { return impl_ ? impl_->stage.get() : nullptr; }
bool Cart::full_canvas() const { return impl_ && impl_->full; }
Hd* Cart::hd() const { return impl_ ? impl_->hd.get() : nullptr; }

std::string Cart::header(std::string_view src, std::string_view key) {
  size_t pos = 0;
  for (int line = 0; line < 12 && pos < src.size(); ++line) {
    size_t nl = src.find('\n', pos);
    if (nl == std::string_view::npos) nl = src.size();
    std::string_view l = src.substr(pos, nl - pos);
    pos = nl + 1;
    if (l.substr(0, 2) != "--") continue;
    l.remove_prefix(2);
    while (!l.empty() && l.front() == ' ') l.remove_prefix(1);
    if (l.substr(0, key.size()) != key) continue;
    l.remove_prefix(key.size());
    while (!l.empty() && l.front() == ' ') l.remove_prefix(1);
    if (l.empty() || l.front() != ':') continue;
    l.remove_prefix(1);
    while (!l.empty() && (l.front() == ' ' || l.front() == '\t')) l.remove_prefix(1);
    while (!l.empty() && (l.back() == ' ' || l.back() == '\r' || l.back() == '\t')) l.remove_suffix(1);
    return std::string(l);
  }
  return {};
}

void Cart::stop(std::string why) {
  error_ = std::move(why);
  // The picture says so (and stays until the cart is closed).
  if (!impl_ || !fb_) return;
  Impl& I = *impl_;
  I.reset_draw();
  shifted_ = false;  // the message shows as it's drawn, in the cart's own colors
  std::memset(shift_, 0, sizeof shift_);
  if (std::any_of(std::begin(row_mask_), std::end(row_mask_), [](uint16_t m) { return m != 0; })) {
    std::memset(row_mask_, 0, sizeof row_mask_);
    ++rows_ver_;
  }
  std::memset(fb_, 1, kW * kH);
  I.print("CART STOPPED", 12, 4, 4, 8);
  // The first line of the message (the traceback goes to the log), wrapped at 30 characters: at
  // a space when the line has one, else where it must.
  std::string msg = error_.substr(0, error_.find('\n'));
  int y = 16;
  for (size_t i = 0; i < msg.size() && y < kH - 20; y += 7) {
    size_t n = std::min<size_t>(30, msg.size() - i);
    if (i + n < msg.size()) {
      const size_t sp = msg.rfind(' ', i + n);
      if (sp != std::string::npos && sp > i) n = sp - i;
    }
    I.print(msg.data() + i, n, 4, y, 7);
    i += n;
    while (i < msg.size() && msg[i] == ' ') ++i;
  }
  I.print("PWR TO LEAVE", 12, 4, kH - 10, 6);
}

void Cart::interrupt() {
  // Past the deadline the cart may still run kDeadlineGrace instructions (a few milliseconds of Lua):
  // an endless loop spends them at once and stops; a frame that was only held up (the board writing
  // flash stalls both cores, and with them a frame mid-draw) finishes.
  if (L_ && in_call_) lua_sethook(L_, deadline_hook, LUA_MASKCOUNT, kDeadlineGrace);
}

bool Cart::run(int nargs, int64_t limit_us, const char* where, std::string* failed) {
  Impl& I = *impl_;
  I.limit_us = limit_us;
  I.frame_t0 = host_.now_us();
  lua_pushcfunction(L_, traceback);
  lua_insert(L_, -(nargs + 2));
  const int base = lua_gettop(L_) - nargs - 1;
  in_call_ = true;
  host_.deadline(limit_us);
  if (host_.frame_budget() > 0) {
    lua_sethook(L_, count_hook, LUA_MASKCOUNT, kCountEvery);
    if (I.budget && I.count_left > 0) L_->hookcount = I.count_left;  // one count across the frame, as check.py's
  } else if (host_.deadline_by_polling()) lua_sethook(L_, poll_hook, LUA_MASKCOUNT, kPollEvery);
  const int rc = lua_pcall(L_, nargs, 0, base);
  in_call_ = false;  // an interrupt from here on is the finished call's: ignored
  host_.deadline(0);
  if (lua_gethook(L_) == count_hook) I.count_left = L_->hookcount;
  if (lua_gethook(L_)) lua_sethook(L_, nullptr, 0, 0);  // one that came as it finished
  lua_remove(L_, base);
  if (rc == LUA_OK) return true;
  std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "error";
  lua_pop(L_, 1);
  if (rc == LUA_ERRMEM) msg = "the cart used more than 1 MB of memory";
  if (failed) {  // the caller decides (reload(): the old code goes on)
    *failed = std::string("in ") + where + ": " + msg;
    return false;
  }
  stop(std::string("in ") + where + ": " + msg);
  return false;
}

bool Cart::start(std::string_view name, std::string_view src) {
  name_ = std::string(name);
  impl_ = new Impl();
  Impl& I = *impl_;
  I.cart = this;
  I.heap.host = &host_;
  I.host = &host_;
  I.reset_draw();
  fb_ = static_cast<uint8_t*>(host_.alloc_fast(kW * kH));
  if (!fb_) {
    error_ = "no memory for the picture";
    return false;
  }
  std::memset(fb_, 0, kW * kH);
  I.fb = fb_;
  S = impl_;
  // The limit doesn't count comments: a source over it is measured (and run) without them, as
  // push.py would have sent it. The carts the firmware carries arrive with theirs.
  std::string stripped;
  if (src.size() > kMaxSource) {
    stripped = shipped(src);
    src = stripped;
  }
  if (src.size() > kMaxSource) {
    stop("the cart is " + std::to_string(src.size()) + " bytes without its comments; the limit is " +
         std::to_string(kMaxSource));
    return false;
  }
  I.caps = host_.caps();
  // "-- canvas: full": the layer covers the whole glass.
  if (const std::string cv = header(src, "canvas"); !cv.empty()) {
    if (cv != "full" && cv != "square") {
      stop("its header says '-- canvas: " + cv + "'; that should be full or square");
      return false;
    }
    I.full = cv == "full";
  }
  // "-- api: N": the API version it needs (a whole number; none: 1).
  if (const std::string api = header(src, "api"); !api.empty()) {
    char* end = nullptr;
    const long v = std::strtol(api.c_str(), &end, 10);
    if (end == api.c_str() || *end != '\0' || v < 1) {
      stop("its header says '-- api: " + api + "'; that should be a whole number (1 to " + std::to_string(kApiVersion) + ")");
      return false;
    }
    if (v > kApiVersion) {
      stop("it needs API " + std::to_string(v) + "; this runtime has " + std::to_string(kApiVersion) + " (update the firmware)");
      return false;
    }
  }
  L_ = lua_newstate(lua_alloc, &I.heap);
  if (!L_) {
    stop("no memory for Lua");
    return false;
  }
  lua_newtable(L_);
  lua_setfield(L_, LUA_REGISTRYINDEX, "hg.data");
  // The libraries a cart gets; then what it mustn't have.
  static const luaL_Reg kLibs[] = {{LUA_GNAME, luaopen_base}, {LUA_STRLIBNAME, luaopen_string},
                                   {LUA_TABLIBNAME, luaopen_table}, {LUA_MATHLIBNAME, luaopen_math},
                                   {LUA_COLIBNAME, luaopen_coroutine}};
  for (const auto& lib : kLibs) {
    luaL_requiref(L_, lib.name, lib.func, 1);
    lua_pop(L_, 1);
  }
  for (const char* g : {"dofile", "loadfile", "load", "require", "collectgarbage", "warn", "print"}) {
    lua_pushnil(L_);
    lua_setglobal(L_, g);
  }
  lua_getglobal(L_, "string");
  lua_pushnil(L_);
  lua_setfield(L_, -2, "dump");
  lua_pop(L_, 1);
  lua_getglobal(L_, "table");
  lua_getfield(L_, -1, "unpack");
  lua_setglobal(L_, "unpack");
  lua_pop(L_, 1);
  static const luaL_Reg kApi[] = {
      {"cls", f_cls},       {"pset", f_pset},   {"pget", f_pget},     {"line", f_line},   {"rect", f_rect},
      {"rectfill", f_rectfill}, {"circ", f_circ}, {"circfill", f_circfill}, {"print", f_print}, {"spr", f_spr}, {"sspr", f_sspr}, {"palt", f_palt}, {"fget", f_fget}, {"fset", f_fset}, {"map", f_map}, {"tline", f_tline}, {"mget", f_mget}, {"mset", f_mset}, {"msize", f_msize},
      {"pal", f_pal},       {"palette", f_palette}, {"rowshift", f_rowshift}, {"rowpal", f_rowpal}, {"camera", f_camera}, {"clip", f_clip},   {"btn", f_btn},     {"btnp", f_btnp},
      {"touch", f_touch},   {"tilt", f_tilt},     {"level", f_level},   {"buzz", f_buzz},     {"sfx", f_sfx},     {"music", f_music}, {"log", f_log}, {"wake", f_wake},
      {"time", f_time},     {"fps", f_fps},     {"rnd", f_rnd},     {"srand", f_srand},   {"flr", f_flr},     {"ceil", f_ceil},
      {"sin", f_sin},       {"cos", f_cos},     {"trifill", f_trifill}, {"abs", f_abs},       {"sgn", f_sgn},     {"min", f_min},       {"max", f_max},     {"mid", f_mid},
      {"save", f_save},     {"load", f_load},     {"now", f_now},       {"date", f_date},   {"data", f_data},     {"act", f_act},     {"stat", f_stat},
      {"shape", f_shape},   {"obj", f_obj},     {"place", f_place},   {"orient", f_orient}, {"hide", f_hide},
      {"paint", f_paint},   {"view", f_view},   {"light", f_light},   {"sky", f_sky},     {"project", f_project},
      {"ground", f_ground}, {"shadow", f_shadow}, {"res", f_res},   {"texture", f_texture}, {"skin", f_skin},
      {"caps", f_caps},
  };
  for (const auto& f : kApi) {
    lua_pushcfunction(L_, f.func);
    lua_setglobal(L_, f.name);
  }
  for (const luaL_Reg* r = host_.extra_api(); r && r->name; ++r) {  // the port's own (Host::extra_api)
    lua_pushcfunction(L_, r->func);
    lua_setglobal(L_, r->name);
  }
  {  // hd: the full-color layer (docs/api.md)
    static const luaL_Reg kHd[] = {
        {"clear", hd_clear},       {"order", hd_order}, {"scanlines", hd_scanlines}, {"rect", hd_rect},
        {"frame", hd_frame},       {"circle", hd_circle}, {"ring", hd_ring},        {"line", hd_line},
        {"gradient", hd_gradient}, {"text", hd_text},   {"measure", hd_measure},    {"wrap", hd_wrap},
        {"clip", hd_clip},         {"scroll", hd_scroll},         {"inset", hd_inset},         {"image", hd_image}, {"images", hd_images},      {nullptr, nullptr},
    };
    luaL_newlib(L_, kHd);
    lua_setglobal(L_, "hd");
  }
  for (const char* f : {"ask", "say"}) {
    lua_pushstring(L_, f);
    lua_pushcclosure(L_, f_not_yet, 1);
    lua_setglobal(L_, f);
  }
  lua_gc(L_, LUA_GCGEN, 0, 0);  // games make short-lived garbage every frame: generational suits it
  I.t0_us = host_.now_us();
  I.rng ^= static_cast<uint32_t>(I.t0_us);
  // ui: the helpers every cart gets (engine/ui.lua), written on the API above.
  if (luaL_loadbufferx(L_, kUiLua, sizeof(kUiLua) - 1, "=ui", "t") != LUA_OK) {
    stop(std::string("the runtime's ui.lua doesn't load: ") + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
    return false;
  }
  if (!run(0, kStartLimitUs, "the runtime's ui.lua")) return false;
  // What reload() needs (its helpers, and what the runtime set up before the cart's code ran).
  if (luaL_loadbufferx(L_, kReloadLua, sizeof(kReloadLua) - 1, "=reload", "t") != LUA_OK) {
    stop(std::string("the runtime's reload code doesn't load: ") + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
    return false;
  }
  static const luaL_Reg kHelpers[] = {{"up", h_up}, {"setup", h_setup}, {"id", h_id}, {"join", h_join},
                                      {"top", h_top}, {"getmt", h_getmt}, {nullptr, nullptr}};
  luaL_newlib(L_, kHelpers);
  lua_pushglobaltable(L_);
  if (lua_pcall(L_, 2, 1, 0) != LUA_OK) {
    stop(std::string("the runtime's reload code doesn't run: ") + (lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?"));
    return false;
  }
  lua_setfield(L_, LUA_REGISTRYINDEX, "luanette.reload");
  call_reload("before", 0);
  if (luaL_loadbufferx(L_, src.data(), src.size(), "=cart", "t") != LUA_OK) {
    std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?";
    lua_pop(L_, 1);
    stop("doesn't load: " + msg);
    return false;
  }
  lua_pushvalue(L_, -1);
  lua_setfield(L_, LUA_REGISTRYINDEX, "luanette.main");
  if (!run(0, kStartLimitUs, "the top of the cart")) return false;
  lua_getfield(L_, LUA_REGISTRYINDEX, "luanette.main");
  reloadable_ = call_reload("begin", 1);  // (a cart that runs out of memory here still runs: it just can't reload)
  lua_getglobal(L_, "_update60");
  update60_ = lua_isfunction(L_, -1);
  lua_pop(L_, 1);
  if (!rate_set_) fps_ = update60_ ? 60 : 30;  // (its top level may have called fps())
  if (lua_getglobal(L_, "_init") == LUA_TFUNCTION) {
    if (!run(0, kStartLimitUs, "_init")) return false;
  } else {
    lua_pop(L_, 1);
  }
  make_shown();
  return true;
}

// R[fn](the nargs values on the stack), R the reload code's table; false (the values popped) when it fails.
bool Cart::call_reload(const char* fn, int nargs) {
  lua_getfield(L_, LUA_REGISTRYINDEX, "luanette.reload");
  lua_getfield(L_, -1, fn);
  lua_remove(L_, -2);
  lua_insert(L_, -(nargs + 1));
  std::string failed;
  return run(nargs, kStartLimitUs, "reload", &failed);
}

std::string Cart::reload(std::string_view src) {
  if (!running()) return "the cart isn't running";
  if (!reloadable_) return "this cart can't take new code while it runs (it ran out of memory as it started)";
  Impl& I = *impl_;
  S = impl_;
  std::string stripped;
  if (src.size() > kMaxSource) {
    stripped = shipped(src);
    src = stripped;
  }
  if (src.size() > kMaxSource) return "the cart is " + std::to_string(src.size()) + " bytes without its comments; the limit is " + std::to_string(kMaxSource);
  if (const std::string api = header(src, "api"); !api.empty() && std::strtol(api.c_str(), nullptr, 10) > kApiVersion)
    return "it needs API " + api + "; this runtime has " + std::to_string(kApiVersion);
  if (luaL_loadbufferx(L_, src.data(), src.size(), "=cart", "t") != LUA_OK) {
    std::string msg = lua_tostring(L_, -1) ? lua_tostring(L_, -1) : "?";
    lua_pop(L_, 1);
    return "doesn't load: " + msg;
  }
  // Its top level runs into a table of its own (reading the cart's globals through it), so a
  // failure there leaves the running cart as it was.
  lua_newtable(L_);  // P
  lua_newtable(L_);
  lua_pushglobaltable(L_);
  lua_setfield(L_, -2, "__index");
  lua_setmetatable(L_, -2);
  lua_pushvalue(L_, -1);
  lua_insert(L_, -3);  // P, chunk, P
  lua_setupvalue(L_, -2, 1);  // the chunk's _ENV
  lua_pushvalue(L_, -1);
  lua_insert(L_, -3);  // chunk, P, chunk
  std::string failed;
  if (!run(0, kStartLimitUs, "the new code's top", &failed)) {
    lua_pop(L_, 2);
    return failed;
  }
  // chunk, P on the stack: merge (a failure from here on leaves the cart half old, half new: it stops)
  if (!call_reload("reload", 2)) {
    stop("in reload: the new code couldn't be merged in (out of memory?)");
    return error_;
  }
  lua_pushnil(L_);
  lua_setfield(L_, LUA_REGISTRYINDEX, "luanette.main");
  I.map_read = false;  // MAP may be another table now
  lua_getglobal(L_, "_update60");
  const bool u60 = lua_isfunction(L_, -1);
  lua_pop(L_, 1);
  if (u60 != update60_) {
    update60_ = u60;
    if (!rate_set_) fps_ = update60_ ? 60 : 30;
  }
  return "";
}

bool Cart::frame(const Input& in) {
  if (!running()) return false;
  Impl& I = *impl_;
  S = impl_;
  I.was = I.in;
  I.in = in;
  const int64_t t0 = host_.now_us();
  const size_t freed0 = I.heap.freed;
  I.ops = I.polls = I.count_left = I.logs = 0;
  I.budget = host_.frame_budget();  // only a frame's calls are held to it (check.py's frame budget)
  bool ok = true;
  for (const char* fn : {update60_ ? "_update60" : "_update", "_draw"}) {
    if (lua_getglobal(L_, fn) != LUA_TFUNCTION) {
      lua_pop(L_, 1);
      continue;
    }
    ok = run(0, std::max<int64_t>(1000, kFrameLimitUs - (host_.now_us() - t0)), fn);  // one limit for both
    if (!ok) break;
  }
  make_shown();
  const int64_t now = host_.now_us();
  last_.lua_us = now - t0;
  last_.freed = I.heap.freed - freed0;
  last_.ops = I.ops;
  I.budget = 0;
  if (idle_) {  // the next frame's rate: active while there's input, and a moment after
    if (in.touching || in.held) hot_until_us_ = now + kLazyHoldUs;
    fps_ = now < hot_until_us_ ? active_ : idle_;
  }
  return ok;
}

void Cart::set_rate(int active, int idle) {
  rate_set_ = true;
  active_ = active;
  idle_ = idle;
  fps_ = idle_ && host_.now_us() >= hot_until_us_ ? idle_ : active_;
}

}  // namespace carts

namespace carts {

void overlay_rows(uint16_t* out, int stride, int x0, int y0, int rows, const uint8_t* pic, const uint16_t* pal, const uint16_t* dim,
                  const uint16_t* const* own) {
  for (int r = 0; r < rows; ++r) {
    const int y = y0 + r;
    const uint8_t* src = pic + (y / 3) * kW;
    const uint16_t* t = own ? own[y / 3] : nullptr;
    const uint16_t* p = t ? (y % 3 == 2 ? t + 16 : t) : y % 3 == 2 ? dim : pal;
    uint16_t* d = out + r * stride + x0;
    const auto* w4 = reinterpret_cast<const uint32_t*>(src);  // clear pixels (most, over a scene) four at a time
    for (int q = 0; q < kW / 4; ++q) {
      if (!w4[q]) continue;
      for (int x = 4 * q; x < 4 * q + 4; ++x) {
        const uint8_t c = src[x] & 15;
        if (c) d[3 * x] = d[3 * x + 1] = d[3 * x + 2] = p[c];
      }
    }
    // The picture's edge columns run on to out's edges (a HUD bar spans the glass, as the scene does)
    if (const uint8_t c = src[0] & 15)
      for (int x = 0; x < x0; ++x) out[r * stride + x] = p[c];
    if (const uint8_t c = src[kW - 1] & 15)
      for (int x = x0 + 3 * kW; x < stride; ++x) out[r * stride + x] = p[c];
  }
}

void Canvas::fill(int c) { std::memset(px, c, static_cast<size_t>(w) * h); }

void Canvas::rectfill(int x0, int y0, int x1, int y1, int c) {
  if (x0 > x1) std::swap(x0, x1);
  if (y0 > y1) std::swap(y0, y1);
  x0 = std::max(x0, 0);
  x1 = std::min(x1, w - 1);
  for (int y = std::max(y0, 0); y <= std::min(y1, h - 1); ++y)
    if (x1 >= x0) std::memset(px + y * w + x0, c, static_cast<size_t>(x1 - x0 + 1));
}

void Canvas::rect(int x0, int y0, int x1, int y1, int c) {
  rectfill(x0, y0, x1, y0, c);
  rectfill(x0, y1, x1, y1, c);
  rectfill(x0, y0, x0, y1, c);
  rectfill(x1, y0, x1, y1, c);
}

void Canvas::circ(int x, int y, int r, int c, bool filled) {
  for (int dy = -r; dy <= r; ++dy)
    for (int dx = -r; dx <= r; ++dx) {
      const int d = dx * dx + dy * dy;
      const int X = x + dx, Y = y + dy;
      if (X < 0 || Y < 0 || X >= w || Y >= h) continue;
      if (filled ? d <= r * r + r : (d >= r * r - r && d <= r * r + r)) px[Y * w + X] = static_cast<uint8_t>(c);
    }
}

int Canvas::print(std::string_view text, int x, int y, int c) {
  for (const char ch0 : text) {
    const unsigned char ch = static_cast<unsigned char>(ch0);
    const uint16_t g = ch < 128 ? kGlyphs.g[ch >= 'a' && ch <= 'z' ? ch - 32 : ch] : kBox;
    for (int k = 0; k < 15; ++k) {
      const int X = x + k % 3, Y = y + k / 3;
      if ((g & (1u << (14 - k))) && X >= 0 && Y >= 0 && X < w && Y < h) px[Y * w + X] = static_cast<uint8_t>(c);
    }
    x += 4;
  }
  return x;
}

}  // namespace carts
