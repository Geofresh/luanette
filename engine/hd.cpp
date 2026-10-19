// The full-colour layer (hd.hpp).
#include "hd.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>

#include "hd_fonts.hpp"
#include "hg/panel_pixels.hpp"

namespace carts {
namespace {

// compose() and front_out() take two pixels' colour (and four alphas) from each 32-bit word of a row.
static_assert(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__, "the layer's row words assume little-endian");

using hd_fonts::Font;
using hd_fonts::Glyph;

inline uint16_t to565(uint32_t rgb) {
  return static_cast<uint16_t>(((rgb >> 19) & 0x1F) << 11 | ((rgb >> 10) & 0x3F) << 5 | ((rgb >> 3) & 0x1F));
}

// d toward s by t/256 (t 0-256), channel by channel.
inline uint16_t lerp565(uint16_t d, uint16_t s, int t) {
  const int dr = d >> 11, dg = (d >> 5) & 0x3F, db = d & 0x1F;
  const int sr = s >> 11, sg = (s >> 5) & 0x3F, sb = s & 0x1F;
  const int r = dr + (((sr - dr) * t) >> 8), g = dg + (((sg - dg) * t) >> 8), b = db + (((sb - db) * t) >> 8);
  return static_cast<uint16_t>(r << 11 | g << 5 | b);
}

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

// floor and ceil to int, inline (on the board std::floor / std::ceil are library calls). For |v| < 2^31:
// the bindings keep coordinates within +-32767.
inline int ifloor(float v) {
  const int i = static_cast<int>(v);
  return i - (v < static_cast<float>(i));
}
inline int iceil(float v) {
  const int i = static_cast<int>(v);
  return i + (v > static_cast<float>(i));
}

// One UTF-8 character from s at i (i moves past it); a broken byte reads as itself.
uint32_t next_cp(std::string_view s, size_t& i) {
  const auto b = static_cast<unsigned char>(s[i++]);
  int extra = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : b >= 0xC0 ? 1 : 0;
  uint32_t cp = extra == 3 ? b & 0x07 : extra == 2 ? b & 0x0F : extra == 1 ? b & 0x1F : b;
  while (extra-- > 0 && i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80)
    cp = cp << 6 | (static_cast<unsigned char>(s[i++]) & 0x3F);
  return cp;
}

const Glyph* find_glyph(const Font& f, uint32_t cp) {
  // Printable ASCII is all there from the space on (tools/hd_fonts.py keeps the glyphs sorted):
  // character c is glyph c - 32, no search.
  if (cp >= 32 && cp < 127 && cp - 32 < f.count && f.glyphs[cp - 32].cp == cp) return &f.glyphs[cp - 32];
  const Glyph* lo = f.glyphs;
  const Glyph* hi = f.glyphs + f.count;
  const Glyph* g = std::lower_bound(lo, hi, cp, [](const Glyph& x, uint32_t c) { return x.cp < c; });
  return g != hi && g->cp == cp ? g : nullptr;
}

const char* font_name(Hd::Font f) { return f == Hd::kBold ? "bold" : f == Hd::kVt ? "vt" : "mono"; }

}  // namespace

Hd::Hd(int w, int h, uint16_t* back_c, uint8_t* back_a, uint16_t* front_c, uint8_t* front_a)
    : w_(std::clamp(w, 1, kMaxW)), h_(std::clamp(h, 1, kMaxH)), pitch_(pitch_of(w_)), bc_(back_c), fc_(front_c), ba_(back_a),
      fa_(front_a), cx1_(w_), cy1_(h_), dirty_((h_ + 31) / 32, 0), sent_((h_ + 31) / 32, 0), visible_((h_ + 31) / 32, 0),
      opaque_((h_ + 31) / 32, 0), front_hash_(h_, 0), solid_((h_ + 31) / 32, 0), inked_((h_ + 31) / 32, 0),
      in_front_(2 * h_, 0) {
  // The back's rows clear (the pool's first half). The front's (its second half) start as nothing to
  // show (visible_ clear: never composed), and a slot from there is written all over before use: no
  // clearing them (half the first hd call's cost, a whole-glass layer's 600 KB of PSRAM writes).
  const size_t n = static_cast<size_t>(pitch_) * h_;
  std::memset(bc_, 0, n * 2);
  std::memset(ba_, 0, n);
  for (int y = 0; y < h_; ++y) {
    drawn_x0_[y] = static_cast<uint16_t>(w_);
    drawn_x1_[y] = sent_x0_[y] = sent_x1_[y] = 0;
    bmap_[y] = static_cast<uint16_t>(y);
    fmap_[y] = static_cast<uint16_t>(h_ + y);
    in_front_[h_ + y] = 1;
  }
  free_.reserve(2 * h_);
  use_vector(true);
  bool vis = false, opq = false;
  front_hash_.assign(h_, row_look(0, &vis, &opq));  // the front rows as they start: all clear, as the back's
}

void Hd::own(int y, bool whole) {
  const uint16_t s = bmap_[y];
  if (!in_front_[s] || front_free_.load(std::memory_order_acquire)) return;  // its own, or nobody reads the front now
  // As many slots are free as rows are shared (2h slots; h front rows and h back rows, distinct
  // within each map): this row is one, so there's one.
  const uint16_t n = free_.back();
  free_.pop_back();
  if (!whole) {
    std::memcpy(slot_c(n), slot_c(s), static_cast<size_t>(pitch_) * 2);
    if (!solid(y)) std::memcpy(slot_a(n), slot_a(s), static_cast<size_t>(pitch_));  // (rule 1: a solid row has no alpha to keep)
  }
  bmap_[y] = n;
}

// The nearest size the font has (a tie goes to the larger).
const hd_fonts::Font* Hd::pick(Font font, int size) const {
  const hd_fonts::Font* best = nullptr;
  for (int i = 0; i < hd_fonts::kFontCount; ++i) {
    const auto& f = hd_fonts::kFonts[i];
    if (std::strcmp(f.name, font_name(font)) != 0) continue;
    if (!best || std::abs(f.size - size) < std::abs(best->size - size) ||
        (std::abs(f.size - size) == std::abs(best->size - size) && f.size > best->size))
      best = &f;
  }
  return best;
}

namespace {
// x / 255 for x up to 255 * 255, and sa * 256 / oa for sa <= oa (the reciprocal rounded up): both
// exact for every input, without the divisions (library calls' worth of cycles on the board).
inline uint32_t div255(uint32_t x) { return (x + 1 + (x >> 8)) >> 8; }
struct Recip {
  uint32_t v[256];
  constexpr Recip() : v() {
    for (uint32_t oa = 1; oa < 256; ++oa) v[oa] = ((256u << 16) + oa - 1) / oa;
  }
};
constexpr Recip kRecip;

// One pixel: colour c at alpha sa (1-255) over what's there (straight alpha, "over").
inline void blend(uint16_t& dc, uint8_t& da, uint16_t c, uint32_t sa) {
  if (sa >= 255 || da == 0) {
    dc = c;
    da = static_cast<uint8_t>(sa > 255 ? 255 : sa);
  } else {  // the result's alpha, and its colour weighted by what each brings
    const uint32_t oa = sa + div255(da * (255 - sa));
    dc = lerp565(dc, c, static_cast<int>((sa * kRecip.v[oa]) >> 16));
    da = static_cast<uint8_t>(oa);
  }
}
}  // namespace

void Hd::solid_out(int y) {
  if (!solid(y)) return;
  own(y);
  std::memset(ba_raw(y), 255, static_cast<size_t>(w_));
  set_bit(solid_, y, false);
}

inline void Hd::put(int y, uint16_t* pc, bool sol, int x, uint16_t c, uint32_t cov) {
  if (sol) {  // over an opaque pixel: blend()'s weight there, alpha stays 255
    pc[x] = cov >= 255 ? c : lerp565(pc[x], c, static_cast<int>((cov * kRecip.v[255]) >> 16));
  } else {
    blend(pc[x], ba_raw(y)[x], c, cov);
  }
}

void Hd::plot(int x, int y, uint16_t c, uint32_t cov) {
  if (cov == 0 || x < cx0_ || x >= cx1_ || y < cy0_ || y >= cy1_) return;
  put(y, wr(y), solid(y), x, c, cov);
  touch_row(y, x, x + 1);
}

// Pixels [x0, x1) of row y wholly covered, at alpha a: filled as a run (PSRAM writes in bursts).
void Hd::span(int y, int x0, int x1, uint16_t c, uint8_t a) {
  if (a == 0 || y < cy0_ || y >= cy1_) return;
  x0 = std::max(x0, cx0_);
  x1 = std::min(x1, cx1_);
  if (x0 >= x1) return;
  uint16_t* pc = wr(y, a == 255 && x0 == 0 && x1 == w_);  // (all of it painted over: no copy of the old row)
  uint8_t* pa = ba_raw(y);
  const bool sol = solid(y);
  if (a == 255) {
    std::fill(pc + x0, pc + x1, c);
    if (x0 == 0 && x1 == w_) set_bit(solid_, y, true);  // the whole row opaque: its alpha needn't be kept
    else if (!sol) std::memset(pa + x0, 255, x1 - x0);
  } else {
    // Over an opaque pixel (alpha stays 255) the colour moves toward c by a / 255: green spread out
    // from red and blue in one 32-bit word, so one multiply weighs all three (to 32 steps).
    const uint32_t cs = (c | static_cast<uint32_t>(c) << 16) & 0x07E0F81F, t = (a * 32 + 127) / 255;
    for (int x = x0; x < x1; ++x) {
      if (sol || pa[x] == 255) {
        const uint32_t d = (pc[x] | static_cast<uint32_t>(pc[x]) << 16) & 0x07E0F81F;
        const uint32_t v = ((cs * t + d * (32 - t)) >> 5) & 0x07E0F81F;
        pc[x] = static_cast<uint16_t>(v | v >> 16);
      } else {
        blend(pc[x], pa[x], c, a);
      }
    }
  }
  touch_row(y, x0, x1);
}

void Hd::span4(int y, int x0, int x1, const uint16_t* pat, uint8_t a) {
  if (a == 0 || y < cy0_ || y >= cy1_) return;
  x0 = std::max(x0, cx0_);
  x1 = std::min(x1, cx1_);
  if (x0 >= x1) return;
  uint16_t* pc = wr(y, a == 255 && x0 == 0 && x1 == w_);
  uint8_t* pa = ba_raw(y);
  if (a == 255) {
    for (int x = x0; x < x1; ++x) pc[x] = pat[x & 3];
    if (x0 == 0 && x1 == w_) set_bit(solid_, y, true);
    else if (!solid(y)) std::memset(pa + x0, 255, static_cast<size_t>(x1 - x0));
  } else if (solid(y)) {
    for (int x = x0; x < x1; ++x) pc[x] = lerp565(pc[x], pat[x & 3], static_cast<int>((a * kRecip.v[255]) >> 16));
  } else {
    for (int x = x0; x < x1; ++x) blend(pc[x], pa[x], pat[x & 3], a);
  }
  touch_row(y, x0, x1);
}

void Hd::cover_row4(int y, float xa, float xb, const uint16_t* pat, uint8_t a) {
  if (y < cy0_ || y >= cy1_ || xb <= xa) return;
  const int ia = iceil(xa), ib = ifloor(xb);
  if (ia >= ib) {
    for (int x = ifloor(xa); x < iceil(xb); ++x) {
      const float cov = clamp01(std::min(x + 1.0f, xb) - std::max(static_cast<float>(x), xa));
      plot(x, y, pat[x & 3], static_cast<uint32_t>(cov * a + 0.5f));
    }
    return;
  }
  if (xa < ia) plot(ia - 1, y, pat[(ia - 1) & 3], static_cast<uint32_t>((ia - xa) * a + 0.5f));
  span4(y, ia, ib, pat, a);
  if (xb > ib) plot(ib, y, pat[ib & 3], static_cast<uint32_t>((xb - ib) * a + 0.5f));
}

void Hd::cover_row(int y, float xa, float xb, uint16_t c, uint8_t a) {
  if (y < cy0_ || y >= cy1_ || xb <= xa) return;
  const int ia = iceil(xa), ib = ifloor(xb);
  if (ia >= ib) {  // within a pixel or two: each measured
    const int x0 = ifloor(xa), x1 = iceil(xb);
    for (int x = x0; x < x1; ++x) {
      const float cov = clamp01(std::min(x + 1.0f, xb) - std::max(static_cast<float>(x), xa));
      plot(x, y, c, static_cast<uint32_t>(cov * a + 0.5f));
    }
    return;
  }
  // The partly covered ends, and the whole pixels between them as one run.
  if (xa < ia) plot(ia - 1, y, c, static_cast<uint32_t>((ia - xa) * a + 0.5f));
  span(y, ia, ib, c, a);
  if (xb > ib) plot(ib, y, c, static_cast<uint32_t>((xb - ib) * a + 0.5f));
}

void Hd::clear(uint32_t rgb, uint8_t a) {
  const uint16_t c = to565(rgb);
  const bool whole = cx0_ == 0 && cx1_ == w_;
  for (int y = cy0_; y < cy1_; ++y) {
    // A row with nothing drawn on it since it was last cleared to nothing stays as it is.
    if (whole && a == 0 && !(inked_[y >> 5] >> (y & 31) & 1)) continue;
    uint16_t* pc = wr(y, whole);  // (across: colour and alpha all written, no copy of the old row)
    std::fill(pc + cx0_, pc + cx1_, c);
    if (whole && a == 255) {
      set_bit(solid_, y, true);
    } else {
      if (whole) set_bit(solid_, y, false);  // (all of its alpha is written just below)
      else solid_out(y);
      std::memset(ba_raw(y) + cx0_, a, cx1_ - cx0_);
    }
    touch_row(y, cx0_, cx1_);
    if (whole && a == 0) inked_[y >> 5] &= ~(1u << (y & 31));
  }
}

void Hd::rect(float x, float y, float w, float h, uint32_t rgb, uint8_t a, float radius) {
  if (w <= 0 || h <= 0) return;
  const uint16_t c = to565(rgb);
  const float y1 = y + h, r = std::min(radius, std::min(w, h) / 2);
  for (int py = std::max(ifloor(y), cy0_); py < std::min(iceil(y1), cy1_); ++py) {
    const float vcov = clamp01(std::min(py + 1.0f, y1) - std::max(static_cast<float>(py), y));
    float inset = 0;
    if (r > 0) {  // the corners: the span narrows by the circle's inset at this row's middle
      const float my = py + 0.5f, dy = std::max(y + r - my, my - (y1 - r));
      if (dy > 0) inset = r - std::sqrt(std::max(0.0f, r * r - dy * dy));
    }
    cover_row(py, x + inset, x + w - inset, c, static_cast<uint8_t>(a * vcov + 0.5f));
  }
}

void Hd::frame(float x, float y, float w, float h, uint32_t rgb, float width, float radius, uint8_t a) {
  if (w <= 0 || h <= 0 || width <= 0) return;
  if (width * 2 >= std::min(w, h)) {
    rect(x, y, w, h, rgb, a, radius);
    return;
  }
  const uint16_t c = to565(rgb);
  const float y1 = y + h, r = std::min(radius, std::min(w, h) / 2), ri = std::max(0.0f, r - width);
  const float iy0 = y + width, iy1 = y1 - width;
  auto inset_at = [](float my, float top, float bottom, float rad) {
    if (rad <= 0) return 0.0f;
    const float dy = std::max(top + rad - my, my - (bottom - rad));
    return dy > 0 ? rad - std::sqrt(std::max(0.0f, rad * rad - dy * dy)) : 0.0f;
  };
  for (int py = std::max(ifloor(y), cy0_); py < std::min(iceil(y1), cy1_); ++py) {
    const float my = py + 0.5f;
    const uint8_t ra = static_cast<uint8_t>(a * clamp01(std::min(py + 1.0f, y1) - std::max(static_cast<float>(py), y)) + 0.5f);
    const float o = inset_at(my, y, y1, r);
    if (my <= iy0 || my >= iy1) {  // the top and bottom bands
      cover_row(py, x + o, x + w - o, c, ra);
    } else {
      const float i = inset_at(my, iy0, iy1, ri);
      cover_row(py, x + o, x + width + i, c, ra);
      cover_row(py, x + w - width - i, x + w - o, c, ra);
    }
  }
}

void Hd::circle(float cx, float cy, float r, uint32_t rgb, uint8_t a) { ring(cx, cy, r, rgb, r + 1, a); }

void Hd::ring(float cx, float cy, float r, uint32_t rgb, float width, uint8_t a) {
  if (r <= 0 || width <= 0) return;
  const uint16_t c = to565(rgb);
  const float ri = r - width;  // the inner edge (below 0: a filled circle)
  // A pixel's coverage: clamp01(r + 0.5 - d) less clamp01(ri + 0.5 - d), d its centre's distance.
  // Each row splits by |dx| into runs: whole (one fill), empty (the hole: skipped) and the edges
  // between, measured pixel by pixel. The runs' bounds keep 0.01 px clear of the exact ones, so a
  // pixel in a run is one the measure would give exactly 1 or 0.
  auto half = [](float rad, float dy) {  // the half-width of a disc of radius rad at dy, or -1
    const float q = rad * rad - dy * dy;
    return q > 0 ? std::sqrt(q) : -1.0f;
  };
  auto lo = [&](float t) { return ifloor(cx - t - 0.5f) + 1; };  // the pixels whose |dx| < t: [lo, hi)
  auto hi = [&](float t) { return iceil(cx + t - 0.5f); };
  // Only rows and columns within the clip are walked (a huge circle costs its visible part).
  for (int py = std::max(ifloor(cy - r - 1), cy0_); py < std::min(iceil(cy + r + 1) + 1, cy1_); ++py) {
    const float dy = py + 0.5f - cy;
    const float outer = half(r + 0.5f, dy);
    if (outer < 0) continue;
    bool drawn = false;
    const float whole = half(r - 0.5f, dy), in = ri > 0 ? half(ri + 0.5f, dy) : -1.0f, hole = ri > 0.5f ? half(ri - 0.5f, dy) : -1.0f;
    const int o0 = std::max(lo(outer + 0.01f), cx0_), o1 = std::min(hi(outer + 0.01f), cx1_);
    const int w0 = whole > 0.01f ? lo(whole - 0.01f) : 0, w1 = whole > 0.01f ? hi(whole - 0.01f) : 0;
    const int i0 = in > 0 ? lo(in + 0.01f) : 0, i1 = in > 0 ? hi(in + 0.01f) : 0;  // touched by the hole
    const int h0 = hole > 0.01f ? lo(hole - 0.01f) : 0, h1 = hole > 0.01f ? hi(hole - 0.01f) : 0;
    for (int px = o0; px < o1;) {
      if (px >= w0 && px < w1 && !(px >= i0 && px < i1)) {
        const int e = i1 > i0 && px < i0 ? std::min(w1, i0) : w1;
        span(py, px, e, c, a);
        px = e;
        continue;
      }
      if (px >= h0 && px < h1) {
        px = h1;
        continue;
      }
      const float dx = px + 0.5f - cx, d = std::sqrt(dx * dx + dy * dy);
      const float cov = clamp01(r + 0.5f - d) - (ri > 0 ? clamp01(ri + 0.5f - d) : 0);
      const uint32_t cv = static_cast<uint32_t>(cov * a + 0.5f);
      if (cv) {
        put(py, wr(py), solid(py), px, c, cv);  // (the row asked each time: a span just before may have taken or filled it)
        drawn = true;
      }
      ++px;
    }
    if (drawn) touch_row(py, o0, o1);
  }
}

void Hd::line(float x0, float y0, float x1, float y1, uint32_t rgb, float width, uint8_t a) {
  const uint16_t c = to565(rgb);
  const float hw = std::max(width, 0.5f) / 2, ex = hw + 1;
  const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy, len = std::sqrt(len2);
  const float inv_len2 = len2 > 0 ? 1 / len2 : 0, inv_len = len > 0 ? 1 / len : 0;
  const int ya = ifloor(std::min(y0, y1) - ex), yb = iceil(std::max(y0, y1) + ex);
  const float bx0 = std::min(x0, x1) - ex, bx1 = std::max(x0, x1) + ex;
  // A pixel has coverage only within hw + 0.5 of the segment: across a row, that's hw + 0.5 over
  // the line's slope either side of where it crosses (and past an end, nearer than that to it).
  const float reach = hw + 0.5f + 0.01f;
  for (int py = std::max(ya, cy0_); py < std::min(yb + 1, cy1_); ++py) {
    const float my = py + 0.5f;
    float xa = bx0, xb = bx1;
    if (std::fabs(dy) > 0.01f) {
      const float t = (my - y0) / dy, xm = x0 + t * dx, spread = reach * len / std::fabs(dy);
      xa = std::max(bx0, xm - spread - 0.5f);  // pixels whose centre (px + 0.5) is within spread of xm
      xb = std::min(bx1, xm + spread - 0.5f);
    }
    const int pa = std::max(ifloor(xa), cx0_), pb = std::min(iceil(xb), cx1_);
    if (pa >= pb) continue;
    auto coverage = [&](int px) {  // the pixel's coverage, from its centre's distance to the segment
      const float mx = px + 0.5f;
      const float t = ((mx - x0) * dx + (my - y0) * dy) * inv_len2;
      float dist;
      if (t > 0 && t < 1) {  // beside the segment: the distance to its line (no square root)
        dist = std::fabs((mx - x0) * dy - (my - y0) * dx) * inv_len;
      } else {  // past an end: to the end
        const float qx = (t <= 0 ? x0 : x1) - mx, qy = (t <= 0 ? y0 : y1) - my;
        dist = std::sqrt(qx * qx + qy * qy);
      }
      return static_cast<uint32_t>(clamp01(hw + 0.5f - dist) * a + 0.5f);
    };
    // A horizontal line (a rule, a divider: the commonest kind): pixels whose centres lie half a
    // pixel inside its ends are beside it, all at the same distance, so one coverage serves the row
    // between its ends; only the ends are measured pixel by pixel. (The same sums as coverage().)
    int ia = pb, ib = pb;
    uint32_t mid = 0;
    if (dy == 0 && dx != 0) {
      ia = std::max(pa, iceil(std::min(x0, x1)));       // px + 0.5 >= min x + 0.5
      ib = std::min(pb, ifloor(std::max(x0, x1) - 1) + 1);  // px + 0.5 <= max x - 0.5
      if (ia < ib) mid = coverage(ia);
      else ia = ib = pb;
    }
    uint16_t* pc = nullptr;  // the row, owned once something lands on it
    const bool sol = solid(py);
    for (int px = pa; px < pb; ++px) {
      if (px == ia) {  // the run between the ends
        if (mid) {
          if (!pc) pc = wr(py);
          for (; px < ib; ++px) put(py, pc, sol, px, c, mid);
        }
        px = ib - 1;
        continue;
      }
      const uint32_t cov = coverage(px);
      if (!cov) continue;
      if (!pc) pc = wr(py);
      put(py, pc, sol, px, c, cov);
    }
    if (pc) touch_row(py, pa, pb);
  }
}

void Hd::gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom, uint8_t a) {
  if (w <= 0 || h <= 0) return;
  if (top == bottom) {  // one colour: a box
    rect(x, y, w, h, top, a);
    return;
  }
  // RGB565 has 32 steps of red and blue: a slow dark gradient bands. Each row is ordered-dithered
  // (4 x 4 Bayer, as carts/pack_images.py does photos): four colours a row, repeating, so filling
  // it stays a run of stores.
  static constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  const float y1 = y + h;
  for (int py = std::max(ifloor(y), cy0_); py < std::min(iceil(y1), cy1_); ++py) {
    const float t = clamp01((py + 0.5f - y) / h);
    auto ch = [&](int s, int max) {  // channel s of the row's colour, in [0, max]
      return (((top >> s) & 255) * (1 - t) + ((bottom >> s) & 255) * t) * max / 255.0f;
    };
    const float r = ch(16, 31), gg = ch(8, 63), bl = ch(0, 31);
    uint16_t pat[4];
    for (int k = 0; k < 4; ++k) {
      const float th = (kBayer[py & 3][k] + 0.5f) / 16;
      pat[k] = static_cast<uint16_t>(std::min(31, static_cast<int>(r + th)) << 11 | std::min(63, static_cast<int>(gg + th)) << 5 |
                                     std::min(31, static_cast<int>(bl + th)));
    }
    const float vcov = clamp01(std::min(py + 1.0f, y1) - std::max(static_cast<float>(py), y));
    cover_row4(py, x, x + w, pat, static_cast<uint8_t>(a * vcov + 0.5f));
  }
}

float Hd::measure(std::string_view s, int size, Font font, float* height) const {
  const hd_fonts::Font* f = pick(font, size);
  if (!f) return 0;
  if (height) *height = static_cast<float>(f->ascent + f->descent);
  float w = 0;
  for (size_t i = 0; i < s.size();) {
    const Glyph* g = find_glyph(*f, next_cp(s, i));
    w += g ? g->adv : f->size / 2;
  }
  return w;
}

float Hd::text(std::string_view s, float x, float y, uint32_t rgb, int size, Font font, Align align, uint8_t a) {
  const hd_fonts::Font* f = pick(font, size);
  if (!f) return 0;
  const float w = measure(s, size, font);
  if (align == kCenter) x -= w / 2;
  if (align == kRight) x -= w;
  const uint16_t c = to565(rgb);
  // The 16 coverage steps at this alpha (as plot() takes them): worked out once, not a division a pixel.
  uint32_t cov[16];
  for (uint32_t k = 0; k < 16; ++k) cov[k] = k * 17 * a / 255;
  int pen = static_cast<int>(std::lround(x));
  const int base = static_cast<int>(std::lround(y)) + f->ascent;
  for (size_t i = 0; i < s.size();) {
    const uint32_t cp = next_cp(s, i);
    const Glyph* g = find_glyph(*f, cp);
    if (!g) {  // not in the font: a small box
      const int bw = f->size / 2;
      frame(static_cast<float>(pen + 1), static_cast<float>(base - f->ascent + 2), static_cast<float>(bw - 2),
            static_cast<float>(f->ascent - 2), rgb, 1, 0, a);
      pen += bw;
      continue;
    }
    // Row by row, each row's pointers, opacity and clip once (as plot() would per pixel).
    const uint8_t* bits = f->bits + g->off;
    const int stride = (g->w + 1) / 2, gx = pen + g->x;
    const int c0 = std::max(0, cx0_ - gx), c1 = std::min(static_cast<int>(g->w), cx1_ - gx);
    for (int r = 0; r < g->h && c0 < c1; ++r) {
      const int py = base + g->y + r;
      if (py < cy0_ || py >= cy1_) continue;
      const uint8_t* row = bits + r * stride;
      uint16_t* pc = nullptr;  // the row, owned once something lands on it
      const bool sol = solid(py);
      for (int col = c0; col < c1; ++col) {
        const uint32_t k = col & 1 ? row[col / 2] & 0x0F : row[col / 2] >> 4;
        const uint32_t cv = cov[k];
        if (!cv) continue;
        if (!pc) pc = wr(py);
        put(py, pc, sol, gx + col, c, cv);
      }
      if (pc) touch_row(py, gx + c0, gx + c1);
    }
    pen += g->adv;
  }
  return w;
}

std::vector<std::string> Hd::wrap(std::string_view s, float width, int size, Font font) const {
  std::vector<std::string> lines;
  std::string line_, word;
  auto fits = [&](const std::string& t) { return measure(t, size, font) <= width; };
  auto flush_word = [&]() {
    if (word.empty()) return;
    const std::string with = line_.empty() ? word : line_ + " " + word;
    if (fits(with)) {
      line_ = with;
    } else {
      if (!line_.empty()) lines.push_back(line_);
      line_.clear();
      // a word wider than the line is broken where it must be
      std::string part;
      for (size_t i = 0; i < word.size();) {
        const size_t start = i;
        next_cp(word, i);
        const std::string more = part + word.substr(start, i - start);
        if (!part.empty() && !fits(more)) {
          lines.push_back(part);
          part = word.substr(start, i - start);
        } else {
          part = more;
        }
      }
      line_ = part;
    }
    word.clear();
  };
  for (char ch : s) {
    if (ch == ' ') {
      flush_word();
    } else if (ch == '\n') {
      flush_word();
      lines.push_back(line_);
      line_.clear();
    } else {
      word += ch;
    }
  }
  flush_word();
  if (!line_.empty() || lines.empty()) lines.push_back(line_);
  return lines;
}

bool hd_image_info(const uint8_t* p, size_t n, int* w, int* h, bool* alpha) {
  if (n < 12 || std::memcmp(p, "HDI1", 4) != 0) return false;
  *w = p[4] | p[5] << 8;
  *h = p[6] | p[7] << 8;
  *alpha = p[8] & 1;
  return *w > 0 && *h > 0 && *w <= Hd::kMaxW && *h <= Hd::kMaxH;
}

bool hd_image_decode(const uint8_t* p, size_t n, HdImage& img) {
  int w = 0, h = 0;
  bool alpha = false;
  if (!hd_image_info(p, n, &w, &h, &alpha) || w != img.w || h != img.h) return false;
  const size_t total = static_cast<size_t>(w) * h, px_bytes = alpha ? 3 : 2;
  size_t i = 12, k = 0;
  bool opaque = true;
  while (k < total) {
    if (i >= n) return false;
    const uint8_t t = p[i++];
    const size_t count = (t & 0x7F) + 1u;
    if (k + count > total) return false;
    if (t & 0x80) {  // one pixel, count times
      if (i + px_bytes > n) return false;
      const uint16_t c = static_cast<uint16_t>(p[i] | p[i + 1] << 8);
      const uint8_t a = alpha ? p[i + 2] : 255;
      i += px_bytes;
      std::fill_n(img.c + k, count, c);
      std::memset(img.a + k, a, count);
      opaque &= a == 255;
    } else {  // count pixels as they are
      if (i + px_bytes * count > n) return false;
      for (size_t j = 0; j < count; ++j, i += px_bytes) {
        img.c[k + j] = static_cast<uint16_t>(p[i] | p[i + 1] << 8);
        img.a[k + j] = alpha ? p[i + 2] : 255;
        opaque &= img.a[k + j] == 255;
      }
    }
    k += count;
  }
  img.opaque = opaque;
  return true;
}

long hd_bundle_index(const uint8_t* p, size_t n, std::vector<HdBundleEntry>& out) {
  out.clear();
  if (n < 8) return 8;
  if (std::memcmp(p, "HDB1", 4) != 0) return -1;
  const uint32_t count = p[4] | p[5] << 8 | p[6] << 16 | static_cast<uint32_t>(p[7]) << 24;
  if (count > 4096) return -1;
  size_t i = 8;
  for (uint32_t k = 0; k < count; ++k) {
    if (i + 1 > n) return static_cast<long>(i + 1 + 255 + 8);  // enough for this entry at most
    const size_t len = p[i];
    if (i + 1 + len + 8 > n) return static_cast<long>(i + 1 + len + 8 + (count - k - 1) * (1 + 255 + 8));
    HdBundleEntry e;
    e.name.assign(reinterpret_cast<const char*>(p + i + 1), len);
    const uint8_t* q = p + i + 1 + len;
    e.off = q[0] | q[1] << 8 | q[2] << 16 | static_cast<uint32_t>(q[3]) << 24;
    e.len = q[4] | q[5] << 8 | q[6] << 16 | static_cast<uint32_t>(q[7]) << 24;
    out.push_back(std::move(e));
    i += 1 + len + 8;
  }
  return 0;
}

void Hd::image(const HdImage& img, float x, float y, float w, float h, uint8_t a) {
  if (a == 0 || !img.c) return;
  const int x0 = static_cast<int>(std::lround(x)), y0 = static_cast<int>(std::lround(y));
  const int iw = w > 0 ? static_cast<int>(std::lround(w)) : img.w, ih = h > 0 ? static_cast<int>(std::lround(h)) : img.h;
  if (iw <= 0 || ih <= 0) return;
  const int xa = std::max(x0, cx0_), xb = std::min(x0 + iw, cx1_);
  const int ya = std::max(y0, cy0_), yb = std::min(y0 + ih, cy1_);
  if (xa >= xb || ya >= yb) return;
  // Nearest sampling at each destination pixel's centre: the source column of every visible
  // column worked out once (32-bit maths: sizes are at most 32767 x 384; a 64-bit divide per
  // pixel is a library call on the board).
  uint16_t col[kMaxW];
  const bool same_w = iw == img.w;
  for (int px = xa; px < xb; ++px) col[px - xa] = static_cast<uint16_t>(same_w ? px - x0 : (2 * (px - x0) + 1) * img.w / (2 * iw));
  const bool plain = img.opaque && a == 255;
  for (int py = ya; py < yb; ++py) {
    const int sy = (2 * (py - y0) + 1) * img.h / (2 * ih);
    const uint16_t* sc = img.c + static_cast<size_t>(sy) * img.w;
    const uint8_t* sa = img.a + static_cast<size_t>(sy) * img.w;
    uint16_t* dc = wr(py, plain && xa == 0 && xb == w_);
    uint8_t* da = ba_raw(py);
    if (plain && same_w) {  // a photo at its own size: rows copied
      std::memcpy(dc + xa, sc + (xa - x0), static_cast<size_t>(xb - xa) * 2);
    } else if (plain) {  // scaled: sampled, nothing to blend
      for (int px = xa; px < xb; ++px) dc[px] = sc[col[px - xa]];
    } else {
      const bool sol = solid(py);
      for (int px = xa; px < xb; ++px) {
        const int sx = col[px - xa];
        const uint32_t al = a == 255 ? sa[sx] : div255(static_cast<uint32_t>(sa[sx]) * a);
        if (!al) continue;
        if (sol) dc[px] = al >= 255 ? sc[sx] : lerp565(dc[px], sc[sx], static_cast<int>((al * kRecip.v[255]) >> 16));
        else blend(dc[px], da[px], sc[sx], al);
      }
    }
    if (plain) {
      if (xa == 0 && xb == w_) set_bit(solid_, py, true);
      else if (!solid(py)) std::memset(da + xa, 255, static_cast<size_t>(xb - xa));
    }
    touch_row(py, xa, xb);
  }
}

namespace {
// Rows [y0, y1) of anything indexed by row, turned as a scroll by dy turns them: row r takes
// row r - dy's, and the rows that leave one end come back in at the other (the uncovered strip).
template <class Get, class Set>
void turn(int y0, int y1, int dy, Get get, Set set) {
  const int n = y1 - y0, k = ((-dy) % n + n) % n;  // row y0 + i takes row y0 + (i + k) % n
  std::vector<decltype(get(0))> was(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) was[static_cast<size_t>(i)] = get(y0 + i);
  for (int i = 0; i < n; ++i) set(y0 + i, was[static_cast<size_t>((i + k) % n)]);
}
}  // namespace

void Hd::scroll(int x, int y, int w, int h, int dy) {
  const int x0 = std::clamp(x, 0, w_), x1 = std::clamp(x + w, x0, w_);
  const int y0 = std::clamp(y, 0, h_), y1 = std::clamp(y + h, y0, h_);
  if (dy == 0 || x0 >= x1 || y0 >= y1 || std::abs(dy) >= y1 - y0) return;
  for (int r = y0; r < y1; ++r) set_bit(inked_, r, true);
  if (x0 == 0 && x1 == w_) {
    // The whole width: the row map turns (no pixel moves), the rows' dirty marks with it; the
    // front buffer turns the same way at publish(). The uncovered strip holds the rows that left
    // the other end: marked, for the cart draws there.
    turn(y0, y1, dy, [&](int r) { return bmap_[r]; }, [&](int r, uint16_t v) { bmap_[r] = v; });
    turn(y0, y1, dy, [&](int r) { return bit(dirty_, r); }, [&](int r, bool v) { set_bit(dirty_, r, v); });
    turn(y0, y1, dy, [&](int r) { return bit(solid_, r); }, [&](int r, bool v) { set_bit(solid_, r, v); });
    turn(y0, y1, dy, [&](int r) { return drawn_x0_[r]; }, [&](int r, uint16_t v) { drawn_x0_[r] = v; });
    turn(y0, y1, dy, [&](int r) { return drawn_x1_[r]; }, [&](int r, uint16_t v) { drawn_x1_[r] = v; });
    const int s0 = dy < 0 ? y1 + dy : y0, s1 = dy < 0 ? y1 : y0 + dy;
    for (int r = s0; r < s1; ++r) touch_row(r, 0, w_);
    moves_.push_back({y0, y1, dy});
    return;
  }
  const size_t n = static_cast<size_t>(x1 - x0);  // a narrower box: its pixels move (alpha too: written out)
  for (int r = y0; r < y1; ++r) solid_out(r);
  auto move = [&](int to, int from) {
    std::memmove(wr(to) + x0, bc(from) + x0, n * 2);
    std::memmove(ba_raw(to) + x0, ba_raw(from) + x0, n);
    touch_row(to, x0, x1);
  };
  if (dy < 0) {  // up: from the top down, so no row is read after it's written
    for (int r = y0; r < y1 + dy; ++r) move(r, r - dy);
  } else {
    for (int r = y1 - 1; r >= y0 + dy; --r) move(r, r - dy);
  }
}

void Hd::clip(float x, float y, float w, float h) {
  cx0_ = std::clamp(ifloor(x), 0, w_);
  cy0_ = std::clamp(ifloor(y), 0, h_);
  cx1_ = std::clamp(iceil(x + w), cx0_, w_);
  cy1_ = std::clamp(iceil(y + h), cy0_, h_);
}

void Hd::unclip() {
  cx0_ = cy0_ = 0;
  cx1_ = w_;
  cy1_ = h_;
}

bool Hd::use_vector(bool on) {
#ifdef HD_PIE
  auto aligned = [](const void* p) { return (reinterpret_cast<uintptr_t>(p) & 15) == 0; };
  pie_ = on && aligned(bc_) && aligned(fc_) && aligned(ba_) && aligned(fa_);
#else
  (void)on;
#endif
  return pie_;
}

uint64_t Hd::row_look(int y, bool* visible, bool* opaque) const {
  uint32_t h1 = 0x811C9DC5, h2 = 0x9E3779B9;
  auto mix = [&](uint32_t v) {
    h1 = (h1 ^ v) * 0x01000193;                           // FNV-1a over words
    h2 = ((h2 ^ v) << 5 | (h2 ^ v) >> 27) + 0x9E3779B9;  // and an xor-rotate-add beside it: one multiply a word
  };
  const auto* cw = static_cast<const unsigned char*>(__builtin_assume_aligned(bc(y), 4));
  for (int i = 0; i < pitch_ / 2; ++i) {
    uint32_t v;
    __builtin_memcpy(&v, __builtin_assume_aligned(cw + 4 * i, 4), 4);
    mix(v);
  }
  if (solid(y)) {  // opaque across, its alpha not kept (rule 1): as 255s would hash
    *visible = *opaque = true;
    mix(0x5011D000u);
  } else {
    const auto* aw = static_cast<const unsigned char*>(__builtin_assume_aligned(ba_raw(y), 4));
    uint32_t ink = 0, all = 0xFFFFFFFFu;
    for (int i = 0; i < pitch_ / 4; ++i) {
      uint32_t v;
      __builtin_memcpy(&v, __builtin_assume_aligned(aw + 4 * i, 4), 4);
      mix(v);
      ink |= v;  // (the pitch's padding is never drawn: 0)
      if (4 * i + 4 <= w_) all &= v;
    }
    for (int x = w_ & ~3; x < w_; ++x) all &= ba_raw(y)[x] == 255 ? 0xFFFFFFFFu : 0;  // a width not a multiple of 4
    *visible = ink != 0;
    *opaque = all == 0xFFFFFFFFu;
  }
  return static_cast<uint64_t>(h1) << 32 | h2;
}

void Hd::set_scanlines(bool on) {
  if (on == scanlines_) return;
  scanlines_ = on;
  resend_ = true;  // rows are dimmed as they go out: every row goes again
}

bool Hd::publish() {
  front_free_.store(false, std::memory_order_relaxed);  // the sender's again (it's idle while this runs)
  bool any = false;
  for (auto& w : sent_) w = 0;
  for (const Move& m : moves_) {  // the scrolls, made on the front's map too (each moved row goes again)
    turn(m.y0, m.y1, m.dy, [&](int r) { return fmap_[r]; }, [&](int r, uint16_t v) { fmap_[r] = v; });
    turn(m.y0, m.y1, m.dy, [&](int r) { return bit(visible_, r); }, [&](int r, bool v) { set_bit(visible_, r, v); });
    turn(m.y0, m.y1, m.dy, [&](int r) { return bit(opaque_, r); }, [&](int r, bool v) { set_bit(opaque_, r, v); });
    turn(m.y0, m.y1, m.dy, [&](int r) { return front_hash_[r]; }, [&](int r, uint64_t v) { front_hash_[r] = v; });
    for (int r = m.y0; r < m.y1; ++r) {
      set_bit(sent_, r, true);
      sent_x0_[r] = 0, sent_x1_[r] = static_cast<uint16_t>(w_);  // (a moved row: all of it)
    }
    any = true;
  }
  moves_.clear();
  for (int w = 0; w < static_cast<int>(dirty_.size()); ++w) {
    uint32_t bits = dirty_[w];
    dirty_[w] = 0;
    while (bits) {
      const int y = w * 32 + __builtin_ctz(bits);
      bits &= bits - 1;
      const uint16_t dx0 = drawn_x0_[y], dx1 = drawn_x1_[y];
      drawn_x0_[y] = static_cast<uint16_t>(w_), drawn_x1_[y] = 0;
      // The row the cart drew becomes the front's (shared: the cart's next drawing on it takes a
      // fresh slot), and the one the front had is free again: no other row holds it (each map's
      // slots are distinct, and a back row shares only the front's same row).
      const uint16_t was = fmap_[y], now = bmap_[y];
      if (was != now) {
        in_front_[was] = 0;
        free_.push_back(was);
        fmap_[y] = now;
        in_front_[now] = 1;
      }
      // The row read once (on the cart's core, after its frame): how it looks, for the sender (all
      // opaque, anything to show), and whether that's new. A row drawn again the same (a cart that
      // draws everything every frame) isn't sent: composing and sending are the costly part, on the
      // core the network shares.
      bool vis = false, opq = false;
      const uint64_t look = row_look(y, &vis, &opq);
      set_bit(visible_, y, vis);
      set_bit(opaque_, y, opq);
      if (look == front_hash_[y] && !resend_) continue;
      front_hash_[y] = look;
      if (bit(sent_, y)) {  // moved too: all of it already
      } else if (dx1 > dx0) {
        sent_x0_[y] = dx0, sent_x1_[y] = dx1;
      } else {  // (a dirty row nothing reports columns for: all of it, to be safe)
        sent_x0_[y] = 0, sent_x1_[y] = static_cast<uint16_t>(w_);
      }
      set_bit(sent_, y, true);
      any = true;
    }
  }
  if (resend_) {
    resend_ = false;
    for (auto& w : sent_) w = ~0u;
    for (int y = 0; y < h_; ++y) sent_x0_[y] = 0, sent_x1_[y] = static_cast<uint16_t>(w_);
    any = true;
  }
  return any;
}

int Hd::verify() {
  // The pool: each map's slots distinct, the front's marked, the free ones in neither map, none lost.
  std::vector<uint8_t> back(2 * h_, 0), front(2 * h_, 0), freed(2 * h_, 0);
  for (int y = 0; y < h_; ++y)
    if (bmap_[y] >= 2 * h_ || back[bmap_[y]]++ || fmap_[y] >= 2 * h_ || front[fmap_[y]]++) return h_;
  for (int y = 0; y < h_; ++y)
    if (bmap_[y] != fmap_[y] && front[bmap_[y]]) return h_;  // a back row on another front row's slot
  for (uint16_t f : free_)
    if (f >= 2 * h_ || freed[f]++ || back[f] || front[f]) return h_;
  for (int s = 0; s < 2 * h_; ++s) {
    if ((in_front_[s] != 0) != (front[s] != 0)) return h_;
    if (!back[s] && !front[s] && !freed[s]) return h_;  // a slot nobody has and not free: lost
  }
  // Every front row with anything to show as the back's (all of the cart's drawing published), and
  // every row's marks right (one with nothing to show may hold anything: it's never composed).
  for (int y = 0; y < h_; ++y) {
    if (bit(visible_, y) && std::memcmp(fc(y), bc(y), static_cast<size_t>(w_) * 2) != 0) return y;
    bool vis = true, opq = true;
    if (!solid(y)) {
      uint32_t ink = 0, all = 0xFF;
      for (int x = 0; x < w_; ++x) {
        ink |= ba_raw(y)[x];
        all &= ba_raw(y)[x];
      }
      vis = ink != 0;
      opq = all == 0xFF;
      if (vis && !bit(opaque_, y) && std::memcmp(fa_raw(y), ba_raw(y), static_cast<size_t>(w_)) != 0) return y;
    }
    if (vis != bit(visible_, y) || opq != bit(opaque_, y)) return y;
  }
  // What the sender is told changed covers all that did: a row's pixels outside its changed_span
  // (all of a row not changed()) are as the last verify saw them (colour; alpha where shown).
  const size_t row = static_cast<size_t>(w_) * 2;  // colour then alpha (as 16-bit), a row
  const bool first = verify_seen_.empty();
  if (first) verify_seen_.assign(row * h_, 0);
  for (int y = 0; y < h_; ++y) {
    uint16_t* seen = verify_seen_.data() + row * y;
    const bool vis = bit(visible_, y), opq = bit(opaque_, y);
    int x0 = 0, x1 = 0;
    if (bit(sent_, y)) changed_span(y, &x0, &x1);
    for (int x = 0; x < w_; ++x) {
      const uint16_t c = vis ? fc(y)[x] : 0, a = !vis ? 0 : opq ? 255 : fa_raw(y)[x];
      if (!first && (x < x0 || x >= x1) && (seen[x] != c || seen[w_ + x] != a)) return y;
      seen[x] = c;
      seen[w_ + x] = a;
    }
  }
  return -1;
}

bool Hd::row_empty(int y) const { return y < 0 || y >= h_ || !(visible_[y >> 5] >> (y & 31) & 1); }

namespace {
// The panel's pixel format (hg/panel_pixels.hpp): big-endian, and a
// scanline row's pixels a quarter darker.
inline uint16_t swap16(uint16_t v) { return hg::panel_order(v); }
inline uint16_t dim1(uint16_t c) { return hg::scan_dim(c); }

// Front pixel c (native order, dimmed already on a scanline row) at alpha a over panel pixel o
// (big-endian). The blend spreads green from red and blue in one 32-bit word: one multiply for
// all three (to 32 steps).
__attribute__((always_inline)) inline void over_px(uint16_t& o, uint16_t c, uint8_t a) {
  if (a == 255) {
    o = swap16(c);
    return;
  }
  constexpr uint32_t kMask = 0x07E0F81F;
  const uint16_t d16 = swap16(o);
  const uint32_t sv = (c | static_cast<uint32_t>(c) << 16) & kMask, dv = (d16 | static_cast<uint32_t>(d16) << 16) & kMask;
  const uint32_t t = (a + 4u) >> 3;  // 0-32
  const uint32_t v = ((sv * t + dv * (32 - t)) >> 5) & kMask;
  o = swap16(static_cast<uint16_t>(v | v >> 16));
}
}  // namespace

#ifdef HD_PIE
extern "C" void hd_pie_over(uint16_t* out, const uint16_t* src, const uint8_t* alpha, int n8, const uint32_t* k, int dim);
namespace {
// hd_pie.S's constants, each broadcast across a register.
alignas(16) constexpr uint32_t kPie[9] = {0x00FF00FF, 0xFF00FF00, 0xF81FF81F, 0x07E007E0, 0x00030003,
                                          0x001F001F, 0x003F003F, 0x00040004, 0xF800F800};
}  // namespace

int hd_pie_selftest(std::string* report) {
  constexpr int kN = 2048;
  auto* src = static_cast<uint16_t*>(aligned_alloc(16, kN * 2));
  auto* out = static_cast<uint16_t*>(aligned_alloc(16, kN * 2));
  auto* got = static_cast<uint16_t*>(aligned_alloc(16, kN * 2));
  auto* alpha = static_cast<uint8_t*>(aligned_alloc(16, kN));
  if (!src || !out || !got || !alpha) {
    free(src), free(out), free(got), free(alpha);
    if (report) *report = "no memory";
    return 1;
  }
  uint32_t r = 12345;
  auto rnd = [&]() { return r = r * 1664525u + 1013904223u; };
  for (int i = 0; i < kN; ++i) {
    src[i] = static_cast<uint16_t>(rnd() >> 16);
    out[i] = static_cast<uint16_t>(rnd() >> 16);
    alpha[i] = i < 256 ? static_cast<uint8_t>(i) : (i & 7) == 0 ? 0 : (i & 7) == 1 ? 255 : static_cast<uint8_t>(rnd() >> 24);
    if (i >= 1024 && i < 1280) alpha[i] = (i >> 3) % 3 == 0 ? 0 : (i >> 3) % 3 == 1 ? 255 : alpha[i];  // whole groups clear (skipped) and opaque
  }
  int bad = 0;
  char line[160];
  for (int dim = 0; dim < 2; ++dim) {
    std::memcpy(got, out, kN * 2);
    hd_pie_over(got, src, alpha, kN / 8, kPie, dim);
    for (int i = 0; i < kN; ++i) {
      uint16_t want = out[i];
      over_px(want, dim ? dim1(src[i]) : src[i], alpha[i]);
      if (got[i] != want && bad++ == 0 && report) {
        snprintf(line, sizeof line, "over dim %d: px %d src %04x out %04x a %u got %04x want %04x", dim, i, src[i], out[i],
                 alpha[i], got[i], want);
        *report = line;
      }
    }
  }
  free(src), free(out), free(got), free(alpha);
  // And as the sender uses them: a layer with see-through shapes, type and edges, its rows composed
  // over the same panel pixels with the vector loops and without, at offsets and widths the sender
  // and the stage use (and odd ones), and baked.
  constexpr int kW = 410, kRows = 12, kP = Hd::pitch_of(kW);
  // (on the heap, all of it: the layer's maps and the rows would crowd the caller's stack)
  auto* planes = static_cast<uint8_t*>(aligned_alloc(16, static_cast<size_t>(kP) * kRows * 6));
  auto* rows = static_cast<uint16_t*>(malloc(3 * 512 * 2));
  auto own = std::make_unique<Hd>(kW, kRows, reinterpret_cast<uint16_t*>(planes), planes + kP * kRows * 2,
                                  reinterpret_cast<uint16_t*>(planes + kP * kRows * 3), planes + kP * kRows * 5);
  if (!planes || !rows) {
    free(planes), free(rows);
    return bad + 1;
  }
  {
    Hd& hd = *own;
    hd.rect(0, 0, kW, 3, 0x203060);                       // opaque rows
    hd.rect(30, 3, 300, 6, 0xff8040, 150, 3);             // see-through, rounded
    hd.circle(200, 6, 5, 0x40ff80, 200);
    hd.text("Wg", 5, 2, 0xffffff, 12, Hd::kMono, Hd::kLeft);  // type's soft edges
    hd.line(0, 10, kW, 11, 0xffec27, 1, 180);
    hd.rect(401, 0, 9, kRows, 0x80c0ff, 90);              // the last columns (the edge carried out)
    hd.publish();
    uint16_t *base = rows, *a = rows + 512, *b = rows + 1024;
    for (int i = 0; i < 512; ++i) base[i] = static_cast<uint16_t>(rnd() >> 16);
    const int cases[][2] = {{0, kW}, {-12, 434}, {-13, 433}, {3, 200}, {8, 64}, {-20, 410}, {40, 17}};
    for (int pass = 0; pass < 2; ++pass) {  // scanlines on, then off
      hd.set_scanlines(pass == 0);
      hd.publish();
      for (int y = 0; y < kRows; ++y) {
        for (const auto& c : cases) {
          std::memcpy(a, base, 512 * 2);
          std::memcpy(b, base, 512 * 2);
          hd.use_vector(true);
          hd.compose(a + 1, y, c[0], c[1]);  // (out not aligned, as a band buffer's rows)
          hd.use_vector(false);
          hd.compose(b + 1, y, c[0], c[1]);
          hd.use_vector(true);
          if (std::memcmp(a, b, 512 * 2) != 0 && bad++ == 0 && report) {
            snprintf(line, sizeof line, "compose row %d x0 %d n %d (scanlines %d)", y, c[0], c[1], pass == 0);
            *report = line;
          }
        }
        hd.use_vector(true);
        hd.front_out(a, y);
        hd.use_vector(false);
        hd.front_out(b, y);
        hd.use_vector(true);
        if (std::memcmp(a, b, kW * 2) != 0 && bad++ == 0 && report) {
          snprintf(line, sizeof line, "front_out row %d (scanlines %d)", y, pass == 0);
          *report = line;
        }
      }
    }
  }
  own.reset();
  free(planes), free(rows);
  return bad;
}
#else
int hd_pie_selftest(std::string*) { return -1; }
#endif

// Front row y's columns [cx, cx + n), baked: byte-swapped for the panel, a scanline row dimmed. The
// sender's rows are this: the row copied out of PSRAM in one burst, then finished in place where it
// goes (hg::finish_run: eight pixels an instruction on the S3's vector unit).
void Hd::bake(uint16_t* out, int y, int cx, int n) const {
  std::memcpy(out, fc(y) + cx, static_cast<size_t>(n) * 2);
  hg::finish_run(out, n, scanlines_ && y % 3 == 2);
}

// Front row y's columns [cx, cx + n) blended over out (the panel's pixels, big-endian) by their
// alpha. In the vector loop (8 pixels at a time, alpha 0 to 255 alike) over the stretch from the
// first column that shows to the last, clear words around it skipped; the rest one by one.
void Hd::blend_over(uint16_t* out, int y, int cx, int n) const {
  const bool dim = scanlines_ && y % 3 == 2;
  const uint16_t* f = fc(y);
  const uint8_t* fa = fa_raw(y);
  auto one = [&](int i) {
    if (const uint8_t a = fa[cx + i]) over_px(out[i], dim ? dim1(f[cx + i]) : f[cx + i], a);
  };
#ifdef HD_PIE
  if (pie_ && n >= 16) {
    // the stretch that shows, in groups of 8 from a multiple of 8: [g0, g1) of out's columns
    int lo = 0, hi = n;
    while (lo < n && !fa[cx + lo]) ++lo;
    while (hi > lo && !fa[cx + hi - 1]) --hi;
    const int g0 = ((cx + lo + 7) & ~7) - cx, g1 = g0 + std::max(0, hi - g0) / 8 * 8;
    if (g1 - g0 >= 16) {
      for (int i = lo; i < g0; ++i) one(i);
      alignas(16) uint16_t row[pitch_of(kMaxW)];
      const int m = g1 - g0;
      std::memcpy(row, out + g0, static_cast<size_t>(m) * 2);
      hd_pie_over(row, f + cx + g0, fa + cx + g0, m / 8, kPie, dim);
      std::memcpy(out + g0, row, static_cast<size_t>(m) * 2);
      for (int i = g1; i < hi; ++i) one(i);
      return;
    }
  }
#endif
  for (int i = 0; i < n; ++i) one(i);
}

void Hd::compose(uint16_t* out, int y, int x0, int n) const {
  if (row_empty(y)) return;
  const bool dim = scanlines_ && y % 3 == 2;
  auto shown = [&](int x) { return dim ? dim1(fc(y)[x]) : fc(y)[x]; };  // a front pixel as it shows (native)
  // out's columns left of the canvas take its column 0 (out to the glass, as the picture's edge
  // columns do: a hard edge would fringe on the panel's subpixels), those right of it its last.
  const int left = std::clamp(-x0, 0, n), right = std::clamp(w_ - x0, left, n);
  if (row_opaque(y)) {  // all of it shows: baked across (its alpha isn't read: it may be stale)
    std::fill_n(out, left, swap16(shown(0)));
    bake(out + left, y, x0 + left, right - left);
    std::fill_n(out + right, n - right, swap16(shown(w_ - 1)));
    return;
  }
  if (const uint8_t a = fa_raw(y)[0])
    for (int i = 0; i < left; ++i) over_px(out[i], shown(0), a);
  if (const uint8_t a = fa_raw(y)[w_ - 1])
    for (int i = right; i < n; ++i) over_px(out[i], shown(w_ - 1), a);
  blend_over(out + left, y, x0 + left, right - left);
}

}  // namespace carts
