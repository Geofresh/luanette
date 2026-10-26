#pragma once
// Carts: small Lua programs that take over the board's screen (a tiny console, PICO-8 flavoured).
// The contract is docs/api.md; tools/check.py runs the same carts on a desktop.
//
// Portable: no ESP-IDF here. The host (on the board, esp32/cart_player.cpp) owns the screen, the input and
// the pacing; a Cart runs the Lua side and draws into a 128x128 picture of palette indices.
// One cart runs at a time. Lua is built with 32-bit integers and floats (the S3's FPU is
// single precision; doubles are software there).

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "hg/json.hpp"

struct lua_State;

namespace carts {

constexpr int kW = 128, kH = 128;
// A 3D cart's scene runs the glass's width (the board's 410 px, the window at x 12 in it): a scene
// that stopped at the window's edge met the black margin in a hard vertical edge, which this
// AMOLED's subpixel layout draws as a colored zig-zag (a full-width scene never has one).
constexpr int kStageW = 410, kStageX = 12;
constexpr size_t kMaxSource = 32 * 1024;  // 16 KB until 2026-10-14: the dice cart outgrew it
// check.py holds carts to 30,000 Lua instructions a frame; the board counts none (a count hook puts
// Lua's VM in trace mode: every instruction pays a call, ~35% of a frame measured). It stops a frame
// that runs too long instead: the host arms a deadline, and Cart::interrupt() hooks the VM then.
constexpr int64_t kFrameLimitUs = 100000;   // a frame (_update + _draw)
constexpr int64_t kStartLimitUs = 2000000;  // the top level, and _init
constexpr int kFrameBudget = 30000;          // Lua instructions a frame, where a host counts them (tools/check.py's BUDGET)
constexpr int kDeadlineGrace = 200000;       // Lua instructions past a limit before the cart is stopped (Cart::interrupt)
constexpr size_t kMaxMemory = 1024 * 1024;
constexpr size_t kMaxSave = 2048;
// now() and date() give nil before this (November 2023): an earlier clock was never set.
constexpr long long kTimeKnownAfter = 1700000000;

enum Button : uint8_t { kLeft, kRight, kUp, kDown, kA, kB, kButtons };

struct Input {
  uint8_t held = 0;          // a bit per Button
  bool touching = false;     // the cart window is touched
  float tx = 0, ty = 0;      // there, in cart pixels
  float ax = 0, ay = 0;      // tilt, about -1..1
};

// The cart API's version (docs/api.md, "Versions"): a cart may ask for one with `-- api: N`, and
// a runtime refuses a cart that needs a newer one. 1: everything before the full-color layer;
// 2: the layer (hd), its images, caps(), touch(true); 3: the cart's own sounds and music (SOUNDS,
// MUSIC, sfx 8-63, music()); 4: log(), and spaces in sprite strings; 5: maps; 6: sprites 0-255,
// spr()'s blocks, sspr(), palt(), sprite flags (FLAGS, fget(), fset(), map()'s layer).
constexpr int kApiVersion = 6;
constexpr int kLogLine = 200;   // a log() line's characters, at most (the rest is cut)
constexpr int kLogLines = 20;   // log() lines a frame, at most (then one saying more were dropped)

struct Sound;  // synth.hpp
struct Song;

// What a cart runs on (caps()).
struct Caps {
  int w = 384, h = 384;  // the cart window, in the panel's pixels
  int window_x = 12, window_y = 60;      // where it sits on the glass
  int glass_w = 410, glass_h = 502;      // the whole glass: a "-- canvas: full" cart's canvas
  int corner = 0;                        // the glass's corners: rounded to this radius (0: square)
  float scale = 1;       // the panel's pixels for each canvas unit
  bool round = false;    // a round window (its corners don't show)
  bool touch = true, tilt = true, buzz = true, sound = false;
  bool hd = true, stage = true;  // the full-color layer, the 3D scene
  bool host = true;              // data() and act() reach a host (an app or a computer feeding it)
  std::string board = "unknown";
};

// What the board (or a host tool) provides.
class Host {
 public:
  virtual ~Host() = default;
  virtual int64_t now_us() = 0;
  // The wall clock for now() and date(): seconds since 1970 (UTC). Tests replay a clock of their own.
  virtual int64_t unix_time() { return static_cast<int64_t>(std::time(nullptr)); }
  // The cart's heap comes in blocks from here (PSRAM on the board), and its picture (fast RAM).
  virtual void* alloc(size_t bytes) = 0;
  virtual void release(void* p) = 0;
  virtual void* alloc_fast(size_t bytes) { return alloc(bytes); }
  virtual void release_fast(void* p) { release(p); }
  virtual void buzz(int ms) = 0;
  virtual void sfx(int n) = 0;
  // The cart's own sounds and music (synth.hpp), for the host's Synth. nullptr stops them; a
  // closing cart stops both. The host keeps what it's given as long as it plays. Hosts without
  // sound leave these.
  virtual void sound(std::shared_ptr<const Sound> s) { (void)s; }
  virtual void music(std::shared_ptr<const Song> song) { (void)song; }
  // A line the cart logged with log(): to the board's serial log, the playground's console, a tool's
  // output. Hosts that show no log leave it.
  virtual void log(std::string_view line) { (void)line; }
  virtual void wake(double secs) = 0;  // light the screen in `secs` (< 0: cancel)
  virtual void level() {}              // tilt() is measured from the pose the board is in now
  virtual void act(std::string_view name, std::string_view arg) = 0;
  // Whether act(name) is one of this host's commands; a cart calling any other gets an error.
  // Each host has its own (Luanette's firmware: "open", "volume", "brightness").
  virtual bool accepts_act(std::string_view name) const { (void)name; return false; }
  // A data channel ("battery", "carts", ...), and a number that changes when it does.
  virtual const hg::json::Value* data(std::string_view name, uint32_t* version) = 0;
  virtual std::string load_save(std::string_view cart) = 0;  // "" when none
  virtual bool store_save(std::string_view cart, std::string_view json) = 0;
  // The cart's images (carts/pack_images.py): one packed image by name into `out`, false when it
  // has none by that name; and their names. Hosts without images (tests) leave them.
  virtual bool image(std::string_view name, std::vector<uint8_t>& out) {
    (void)name, (void)out;
    return false;
  }
  virtual std::vector<std::string> image_names() { return {}; }
  virtual Caps caps() { return {}; }
  // Call Cart::interrupt() if Lua still runs `us` from now (from an interrupt on Lua's own core,
  // or any context that preempts it there); 0 cancels. Hosts without one (tests) leave it.
  virtual void deadline(int64_t us) { (void)us; }
  // A host with no way to interrupt Lua (a browser: one thread, no signals) says so here, and the
  // runtime checks the clock itself every few thousand instructions instead.
  virtual bool deadline_by_polling() const { return false; }
  // Lua instructions a frame (_update and _draw together) before the cart is stopped, counted as
  // tools/check.py counts them (every 100); 0: not counted. The playground counts, so a cart that
  // passes there passes check.py too; the board relies on its time limit instead.
  virtual int frame_budget() const { return 0; }
};

struct Frame {
  int64_t lua_us = 0;   // _update + _draw, wall time
  int ops = 0;          // their Lua instructions, by 100s, where the host counts them (Host::frame_budget)
  size_t freed = 0;     // bytes the Lua heap gave back meanwhile (tens of KB: a collection ran in it)
};

// The 16 colors a cart starts with (the PICO-8 palette), 0xRRGGBB; palette() changes them per cart.
constexpr uint32_t kPalette[16] = {0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
                                   0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA};

class Cart {
 public:
  explicit Cart(Host& host);
  ~Cart();
  Cart(const Cart&) = delete;
  Cart& operator=(const Cart&) = delete;

  // Compiles `src` and runs its top level and _init. False: error() says why (the picture shows it).
  bool start(std::string_view name, std::string_view src);
  // One frame: _update (or _update60), then _draw. False: the cart stopped (error(); picture shows it).
  bool frame(const Input& in);
  bool running() const { return L_ != nullptr && error_.empty(); }
  // The deadline passed (Host::deadline): the VM stops at its next instruction with an error.
  // Async-safe on Lua's own core (an interrupt there), as lua_sethook is.
  void interrupt();
  // The frame rate for the next frame (one that divides 60): _update60's 60 (else 30) until the
  // cart calls fps(); with fps(active, idle), `active` while there's input (a touch, a button) and
  // for kLazyHoldUs after, else `idle`. A port starts a frame at once when input comes to an idle
  // cart (lazy()), so a slow idle rate costs no response time.
  int fps() const { return fps_; }
  bool lazy() const { return idle_ != 0; }
  void set_rate(int active, int idle);  // idle 0: always `active`
  static bool rate_ok(int fps) { return fps >= 1 && fps <= 60 && 60 % fps == 0; }
  static constexpr int64_t kLazyHoldUs = 500000;
  const uint8_t* picture() const { return fb_; }
  // What each of the picture's 16 indices shows (0xRRGGBB), and a count that moves whenever the cart
  // changes one (palette()): a port rebuilds its color tables, and sends every row again, then.
  const uint32_t* colors() const { return rgb_; }
  uint32_t colors_version() const { return colors_ver_; }
  void set_color(int i, uint32_t rgb) {
    if (i < 0 || i > 15 || rgb_[i] == (rgb & 0xFFFFFF)) return;
    rgb_[i] = rgb & 0xFFFFFF;
    ++colors_ver_;
  }
  void reset_colors() {
    for (int i = 0; i < 16; ++i) set_color(i, kPalette[i]);
  }
  // The cart's 3D scene, once it has made anything 3D (docs/api.md, "3D"); then its picture is an
  // overlay: color 0 shows the scene through.
  class Stage* stage() const;
  // The cart's full-color layer, once it has drawn into it (docs/api.md, "hd"); nullptr before.
  class Hd* hd() const;
  // The cart asked for the whole glass ("-- canvas: full"): its layer is Caps::glass_w x glass_h,
  // in glass pixels (the picture still sits in the window); else the 384 x 384 window.
  bool full_canvas() const;
  const std::string& error() const { return error_; }
  const std::string& name() const { return name_; }
  const Frame& last() const { return last_; }
  size_t memory() const;  // bytes the Lua heap holds
  // The real frame rate and frame period, for stat().
  float real_fps() const { return real_fps_; }
  float period_us() const { return period_us_; }
  void set_rates(float fps, float period_us) {
    real_fps_ = fps;
    period_us_ = period_us;
  }

  // The header comments (`-- title: ...`), "" when missing.
  static std::string header(std::string_view src, std::string_view key);

  struct Impl;  // the Lua side's view (cart.cpp)

 private:
  bool run(int nargs, int64_t limit_us, const char* where);
  void stop(std::string why);

  Host& host_;
  lua_State* L_ = nullptr;
  uint8_t* fb_ = nullptr;
  std::string name_, error_;
  int fps_ = 30;
  bool rate_set_ = false, update60_ = false;  // fps() was called; the cart has _update60
  int active_ = 30, idle_ = 0;  // fps(active, idle); idle 0: not lazy
  int64_t hot_until_us_ = 0;    // lazy: `active` until then (the last input, and kLazyHoldUs)
  Frame last_;
  float real_fps_ = 0, period_us_ = 16667;
  volatile bool in_call_ = false;  // Lua runs (interrupt() applies)
  uint32_t rgb_[16] = {0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
                       0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA};
  uint32_t colors_ver_ = 0;
  Impl* impl_ = nullptr;
};

}  // namespace carts

namespace carts {

// A 3D cart's picture over its scene: window rows [y0, y0 + rows) of out (`stride` wide, the
// picture at 3x from column x0), pal and dim (the scanline rows, y % 3 == 2) in out's pixel format.
// Color 0 shows the scene through; the picture's edge columns run on to out's edges.
void overlay_rows(uint16_t* out, int stride, int x0, int y0, int rows, const uint8_t* pic, const uint16_t* pal, const uint16_t* dim);

// Drawing into any 8-bit picture with the carts' own rules and font (the board's touch pad).
struct Canvas {
  uint8_t* px;
  int w, h;
  void fill(int c);
  void rectfill(int x0, int y0, int x1, int y1, int c);
  void rect(int x0, int y0, int x1, int y1, int c);
  void circ(int x, int y, int r, int c, bool fill);
  int print(std::string_view text, int x, int y, int c);
};

}  // namespace carts
