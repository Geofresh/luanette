// Carts' full-color layer (the `hd` API, docs/api.md): text in real fonts, smooth shapes and
// (later) images on a 384 x 384 canvas, kept between frames until cleared, composed with the 3D
// scene and the 16-color picture band by band (over the picture, or under it).
//
// Two planes: RGB565 (native order, not premultiplied) and 8-bit alpha, kept as a pool of rows
// (twice the canvas's) that two maps index: the back rows the cart draws on, and the front rows the
// sender composes from while the next frame draws. publish() copies nothing: a row the cart changed
// becomes the front's row as it stands, shared with the back. Once the sender is done with the front
// (release_front()), the cart draws on shared rows in place; before that (a frame that starts while
// the last still goes out) a shared row it draws on takes a fresh row from the pool first (copy on
// write: no copy at all when the first thing drawn covers the whole row, as a redrawn screen's
// background does). The sender bakes rows for the panel (byte order, scanline dimming) as it copies
// them out. Portable (no ESP-IDF): the desktop runtime uses it too.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hd_fonts.hpp"

namespace carts {

// A cart's image unpacked (carts/pack_images.py's format): w x h of RGB565 (native order) and
// alpha. The planes are the caller's (see hd_image_info for their sizes).
struct HdImage {
  int w = 0, h = 0;
  uint16_t* c = nullptr;
  uint8_t* a = nullptr;
  bool opaque = true;  // every alpha 255 (no alpha channel in the file)
};
// A packed image's size (and whether it has alpha); false if it isn't one.
bool hd_image_info(const uint8_t* p, size_t n, int* w, int* h, bool* alpha);
// Unpacks it into img's planes (img.w x img.h each, from hd_image_info); false if it's damaged.
bool hd_image_decode(const uint8_t* p, size_t n, HdImage& img);

// On the board (HD_PIE: the ESP32-S3's vector instructions, hd_pie.S): its vector loops against the
// scalar code on random pixels, every alpha, dimmed and not. The mismatches (0: bit for bit), and
// the first one described in `report`; -1 where there are no vector loops.
int hd_pie_selftest(std::string* report);

// One image in a bundle (pack_images.py: "HDB1", its index, the images).
struct HdBundleEntry {
  std::string name;
  uint32_t off = 0, len = 0;
};
// The index from the bundle's first n bytes. Returns 0 when it's read, the bytes the index needs
// when n is too few (read that much and call again), or -1 when it isn't a bundle.
long hd_bundle_index(const uint8_t* p, size_t n, std::vector<HdBundleEntry>& out);

// Three rules for anyone changing the layer (they keep bytes off PSRAM; they're traps if forgotten):
//  1. Solid back rows (solid_): a row painted opaque across keeps no alpha. Its alpha bytes are
//     stale: read or write them (ba_raw) only where solid(y) is false, or after solid_out(y),
//     which writes them out as 255. Blends on a solid row treat every pixel as opaque.
//  2. Opaque front rows (opaque_): a row that's opaque across may be a solid one, so the front's
//     alpha for it is stale. Read it (fa_raw) only where row_opaque(y) is false; compose() and the
//     sender check that first.
//  3. Shared rows: a back row may be the front's row too. Write to a back row only through wr(y)
//     (or after own(y)), which gives it a row of its own first while the sender may still read it.
class Hd {
 public:
  static constexpr int kW = 384, kH = 384;        // the square canvas: the cart window
  static constexpr int kMaxW = 410, kMaxH = 502;  // the whole glass ("-- canvas: full")
  static constexpr int pitch_of(int w) { return (w + 7) & ~7; }  // a row's pixels in memory: rows of whole 16-byte lines (the vector loops')
  enum Font : uint8_t { kMono, kBold, kVt };
  enum Align : uint8_t { kLeft, kCenter, kRight };

  // A w x h canvas (at most kMaxW x kMaxH). Its rows' storage: four planes, pitch_of(w) * h pixels
  // each (two of color, two of alpha: the pool's two halves): the caller allocates them (in PSRAM
  // on the board) and keeps them alive while the layer is in use.
  Hd(int w, int h, uint16_t* back_c, uint8_t* back_a, uint16_t* front_c, uint8_t* front_a);
  int width() const { return w_; }
  int height() const { return h_; }

  // ---- drawing (canvas units; colors 0xRRGGBB; alpha 0-255) ----
  void clear(uint32_t rgb = 0, uint8_t a = 0);
  void rect(float x, float y, float w, float h, uint32_t rgb, uint8_t a = 255, float radius = 0);
  void frame(float x, float y, float w, float h, uint32_t rgb, float width = 1, float radius = 0, uint8_t a = 255);
  void circle(float cx, float cy, float r, uint32_t rgb, uint8_t a = 255);
  void ring(float cx, float cy, float r, uint32_t rgb, float width = 1, uint8_t a = 255);
  void line(float x0, float y0, float x1, float y1, uint32_t rgb, float width = 1, uint8_t a = 255);
  void gradient(float x, float y, float w, float h, uint32_t top, uint32_t bottom, uint8_t a = 255);
  // Text from the pen at (x, y) = the top of the line; returns its width. size snaps to the nearest the font has.
  float text(std::string_view s, float x, float y, uint32_t rgb, int size = 16, Font font = kMono, Align align = kLeft,
             uint8_t a = 255);
  float measure(std::string_view s, int size = 16, Font font = kMono, float* height = nullptr) const;
  std::vector<std::string> wrap(std::string_view s, float width, int size = 16, Font font = kMono) const;
  // An image with its top left at (x, y) (rounded to the pixel), w x h (its own size when 0; a
  // different size is sampled nearest), at alpha a over the image's own.
  void image(const HdImage& img, float x, float y, float w, float h, uint8_t a = 255);
  // Moves what's in the box (x, y, w, h) by dy rows (down when positive); the rows it uncovers keep
  // what they had, for the cart to draw again. Scrolling: move what's drawn, draw only the new strip.
  void scroll(int x, int y, int w, int h, int dy);
  void clip(float x, float y, float w, float h);
  void unclip();
  void set_over(bool over) { over_ = over; }
  bool over() const { return over_; }
  void set_scanlines(bool on);
  bool scanlines() const { return scanlines_; }

  // ---- sending ----
  // After the cart's frame, while nobody composes: the rows it changed become the front's (no
  // copy: see above). Returns whether any did; changed() then says which (bit y of the canvas row).
  bool publish();
  // The sender is done with the front rows until the next publish(): the cart may draw on them in
  // place from now (any thread; the next publish() takes them back).
  void release_front() { front_free_.store(true, std::memory_order_release); }
  // For tests (cartrun): -1 when the rows' bookkeeping holds (each row in one place, the pool's
  // free rows free) and every front row is its back row (color; and alpha where it isn't opaque)
  // with the right opaque and visible marks; else the first row that isn't (h for the pool).
  // Call it after publish().
  int verify();
  const std::vector<uint32_t>& changed() const { return sent_; }  // the rows the last publish() moved
  // A row changed()'s columns that may differ, [*x0, *x1) (the columns drawn on since the publish
  // before; the whole row when it moved or was baked anew): the sender sends no more of it.
  void changed_span(int y, int* x0, int* x1) const {
    *x0 = sent_x0_[y];
    *x1 = sent_x1_[y];
  }
  // Blends front row `y`, columns [x0, x0 + n), over `out` (RGB565 in the panel's byte order:
  // big-endian, a scanline row dimmed, as the front row is baked on the way); columns outside the
  // canvas take its edge column (out to the glass).
  void compose(uint16_t* out, int y, int x0, int n) const;
  bool row_empty(int y) const;  // nothing visible in the front buffer's row
  // Every pixel of the front row opaque: then it's all that shows wherever the layer is over the
  // picture (or the picture shows nothing there), and front_out() is the panel's row.
  bool row_opaque(int y) const { return y >= 0 && y < h_ && bit(opaque_, y); }
  void front_out(uint16_t* out, int y) const { bake(out, y, 0, w_); }  // front row y's width() pixels as the panel takes them
  // For hd_pie_selftest: the vector loops off (or back on, where the planes allow); whether they're on.
  bool use_vector(bool on);

 private:
  void plot(int x, int y, uint16_t c565, uint32_t cov255);  // coverage 0-255 x the op's alpha
  // plot() with the row's work done once by the caller (its color row pc = bc(y), solid(y)) and x
  // known to be inside the clip: the inner loops' version. The caller touch_row()s y if it drew.
  inline void put(int y, uint16_t* pc, bool sol, int x, uint16_t c565, uint32_t cov255);
  void touch_row(int y, int x0, int x1) {  // drawn on in columns [x0, x1)
    dirty_[y >> 5] |= 1u << (y & 31);
    inked_[y >> 5] |= 1u << (y & 31);
    if (x0 < drawn_x0_[y]) drawn_x0_[y] = static_cast<uint16_t>(x0);
    if (x1 > drawn_x1_[y]) drawn_x1_[y] = static_cast<uint16_t>(x1);
  }
  void span(int y, int x0, int x1, uint16_t c, uint8_t a);  // whole pixels [x0, x1), the clip applied
  // As span / cover_row, in a color that repeats every 4 pixels (pat[x & 3]: an ordered dither).
  void span4(int y, int x0, int x1, const uint16_t* pat, uint8_t a);
  void cover_row4(int y, float xa, float xb, const uint16_t* pat, uint8_t a);
  void cover_row(int y, float xa, float xb, uint16_t c, uint8_t a);  // a horizontal span, edges smooth
  const hd_fonts::Font* pick(Font font, int size) const;
  // Front row y's columns [cx, cx + n) as the panel takes them (byte-swapped, a scanline row
  // dimmed), into out; and blended over out (the panel's pixels) by their alpha. The vector loops
  // where they apply (HD_PIE), the scalar code for the rest.
  void bake(uint16_t* out, int y, int cx, int n) const;
  void blend_over(uint16_t* out, int y, int cx, int n) const;
  // Back row y read once: a hash of how it looks (color; alpha unless it's solid), whether
  // anything of it shows and whether all of it is opaque.
  uint64_t row_look(int y, bool* visible, bool* opaque) const;

  int w_, h_, pitch_;
  uint16_t *bc_, *fc_;
  uint8_t *ba_, *fa_;
  int cx0_ = 0, cy0_ = 0, cx1_ = 0, cy1_ = 0;  // the clip, [x0, x1) x [y0, y1) (the canvas, from the start)
  bool over_ = true, scanlines_ = true;
  bool resend_ = false;  // scanlines changed: every row goes to the panel again (baked anew) at the next publish()
  std::vector<uint32_t> dirty_, sent_;  // a bit a row: drawn since the last publish / moved by it
  uint16_t drawn_x0_[kMaxH], drawn_x1_[kMaxH];  // each row's columns drawn on since the last publish ([x0, x1); x1 <= x0: none)
  uint16_t sent_x0_[kMaxH], sent_x1_[kMaxH];    // and those of the rows the last publish moved (changed_span)
  std::vector<uint16_t> verify_seen_;           // verify()'s copy of what each row showed (tests only)
  std::vector<uint32_t> visible_;       // a bit a row: the front row holds something
  std::vector<uint32_t> opaque_;        // a bit a row: all of the front row opaque (its alpha then unread)
  std::vector<uint64_t> front_hash_;    // each front row's look (row_look()): a row drawn again the same isn't sent
  // A bit a row of the back buffer: every pixel opaque, and its alpha row not kept (opaque fills,
  // which most redraws are, skip a third of their bytes; blends there skip reading alpha). The
  // alpha row is written out (solid_out) only when something there turns see-through.
  std::vector<uint32_t> solid_;
  bool solid(int y) const { return bit(solid_, y); }
  void solid_out(int y);
  std::vector<uint32_t> inked_;         // a bit a row: drawn on since the back buffer's row was cleared to nothing
  // The pool: 2h rows, slot s in the first two planes (s < h) or the other two. The back's and the
  // front's rows through a map each (canvas row y is slot map[y]): a whole-width scroll turns the
  // maps, moving no pixels; publish() points the front's at the back's slots.
  uint16_t bmap_[kMaxH], fmap_[kMaxH];
  std::vector<uint8_t> in_front_;  // a byte a slot: some front row's (the back may share it, never write it: rule 3)
  std::vector<uint16_t> free_;     // slots nobody's row holds
  std::atomic<bool> front_free_{false};  // release_front() since the last publish(): shared rows drawn on in place
  bool pie_ = false;  // the planes are 16-byte aligned: the vector loops apply
  uint16_t* slot_c(int s) const { return (s < h_ ? bc_ + static_cast<size_t>(s) * pitch_ : fc_ + static_cast<size_t>(s - h_) * pitch_); }
  uint8_t* slot_a(int s) const { return (s < h_ ? ba_ + static_cast<size_t>(s) * pitch_ : fa_ + static_cast<size_t>(s - h_) * pitch_); }
  uint16_t* bc(int y) const { return slot_c(bmap_[y]); }
  uint8_t* ba_raw(int y) const { return slot_a(bmap_[y]); }  // rule 1
  uint16_t* fc(int y) const { return slot_c(fmap_[y]); }
  uint8_t* fa_raw(int y) const { return slot_a(fmap_[y]); }  // rule 2
  // Back row y the cart's own (rule 3): if the front holds its slot and the sender may still read it,
  // a fresh one, with the row copied in unless `whole` (what's drawn next covers all of it).
  void own(int y, bool whole = false);
  uint16_t* wr(int y, bool whole = false) {  // back row y's color, to write (owned first)
    own(y, whole);
    return bc(y);
  }
  struct Move {
    int y0, y1, dy;  // a whole-width scroll of rows [y0, y1) by dy
  };
  std::vector<Move> moves_;  // scrolls since the last publish(), for the front buffer to make too
  bool bit(const std::vector<uint32_t>& v, int y) const { return v[y >> 5] >> (y & 31) & 1; }
  static void set_bit(std::vector<uint32_t>& v, int y, bool on) {
    if (on) v[y >> 5] |= 1u << (y & 31);
    else v[y >> 5] &= ~(1u << (y & 31));
  }
};

}  // namespace carts
