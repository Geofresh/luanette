// Luanette in a browser: the engine built with Emscripten, hosted the way engine/host/cartrun.cpp
// hosts it, but live. The page (web/index.html) calls lw_start with a cart's source, then
// lw_frame at the cart's rate with the input it gathered (touch, keys, tilt); each frame is
// composed as the board composes it (scene, layer, the 3x picture with its scanline rows) into
// one RGBA image of the whole glass, which the page puts on a canvas.
#include <emscripten/emscripten.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "cart.hpp"
#include "hd.hpp"
#include "sfx.hpp"
#include "stage.hpp"

// Saves live in the browser's localStorage, one key per cart.
EM_JS(char*, js_load_save, (const char* cart), {
  const v = localStorage.getItem('luanette.save.' + UTF8ToString(cart)) || '';
  return stringToNewUTF8(v);
});
EM_JS(int, js_store_save, (const char* cart, const char* json), {
  try {
    localStorage.setItem('luanette.save.' + UTF8ToString(cart), UTF8ToString(json));
    return 1;
  } catch (e) {
    return 0;
  }
});

namespace {

constexpr int kW = carts::kStageW;  // the glass, 410 x 502
constexpr int kH = 502;

struct WebHost final : carts::Host {
  std::deque<int> sounds;  // sfx() calls the page hasn't played yet
  int64_t now_us() override { return static_cast<int64_t>(emscripten_get_now() * 1000.0); }
  void* alloc(size_t n) override { return std::malloc(n); }
  void release(void* p) override { std::free(p); }
  void buzz(int) override {}
  void sfx(int n) override {
    if (sounds.size() < 16) sounds.push_back(n);
  }
  void wake(double) override {}
  void act(std::string_view, std::string_view) override {}
  bool accepts_act(std::string_view name) const override {  // the board's (the launcher's), so carts fail the same way
    return name == "open" || name == "volume" || name == "brightness";
  }
  const hg::json::Value* data(std::string_view, uint32_t*) override { return nullptr; }
  carts::Caps caps() override {
    carts::Caps c;  // the AMOLED-2.06's, which the page draws
    c.board = "browser";
    c.corner = 100;
    c.buzz = false;
    c.sound = true;
    return c;
  }
  std::string load_save(std::string_view cart) override {
    char* v = js_load_save(std::string(cart).c_str());
    std::string out(v);
    std::free(v);
    return out;
  }
  bool store_save(std::string_view cart, std::string_view json) override {
    return js_store_save(std::string(cart).c_str(), std::string(json).c_str()) != 0;
  }
  bool deadline_by_polling() const override { return true; }
};

WebHost g_host;
std::unique_ptr<carts::Cart> g_cart;
std::string g_error;
std::vector<uint16_t> g_glass(static_cast<size_t>(kW) * kH);  // RGB565, the panel's byte order
std::vector<uint8_t> g_rgba(static_cast<size_t>(kW) * kH * 4);
std::vector<uint16_t> g_work;
std::vector<int16_t> g_pcm;
uint16_t g_pal[16], g_dim[16];
uint32_t g_pal_ver = UINT32_MAX;

uint16_t pack(uint32_t r, uint32_t g, uint32_t b) {
  const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
  return static_cast<uint16_t>(v >> 8 | v << 8);
}

// The cart's 16 colors, and its scanline rows a quarter darker (as cartrun and the board).
void set_colors(const carts::Cart& c) {
  for (int i = 0; i < 16; ++i) {
    const uint32_t rgb = c.colors()[i];
    g_pal[i] = pack(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
    g_dim[i] = pack((rgb >> 16 & 255) * 3 / 4, (rgb >> 8 & 255) * 3 / 4, (rgb & 255) * 3 / 4);
  }
  g_pal_ver = c.colors_version();
}

// The whole glass as the board shows it: the window's rows band by band (the scene or color 0,
// the layer under, the picture, the layer over), and on a full canvas the rows around it.
void compose() {
  carts::Cart& cart = *g_cart;
  if (cart.colors_version() != g_pal_ver) set_colors(cart);
  carts::Stage* st = cart.stage();
  carts::Hd* hd = cart.hd();
  const carts::Caps caps = g_host.caps();
  const bool full = hd && cart.full_canvas();
  const int H = carts::Stage::kH, top = caps.window_y;
  if (st && g_work.size() < static_cast<size_t>(st->work_cells())) g_work.resize(st->work_cells());
  const int band = st ? st->band_rows() : 16;
  for (int y = 0; y < H; y += band) {
    uint16_t* rows = g_glass.data() + static_cast<size_t>(top + y) * kW;
    if (st) {
      st->band(y, y + band, rows, g_work.data());
    } else {
      for (int r = 0; r < band; ++r) std::fill(rows + r * kW, rows + (r + 1) * kW, (y + r) % 3 == 2 ? g_dim[0] : g_pal[0]);
    }
    auto layer = [&]() {
      for (int r = 0; r < band; ++r) hd->compose(rows + r * kW, (full ? top : 0) + y + r, full ? 0 : -carts::kStageX, kW);
    };
    if (hd && !hd->over()) layer();
    carts::overlay_rows(rows, kW, carts::kStageX, y, band, cart.picture(), g_pal, g_dim);
    if (hd && hd->over()) layer();
  }
  for (int gy = 0; gy < kH; ++gy) {  // around the window: dark, or a full canvas's own rows
    if (gy >= top && gy < top + H) continue;
    uint16_t* row = g_glass.data() + static_cast<size_t>(gy) * kW;
    if (full) {
      std::fill(row, row + kW, gy % 3 == 2 ? g_dim[0] : g_pal[0]);
      hd->compose(row, gy, 0, kW);
    } else {
      std::fill(row, row + kW, 0);
    }
  }
  uint8_t* out = g_rgba.data();
  for (const uint16_t pv : g_glass) {
    const uint16_t v = static_cast<uint16_t>(pv >> 8 | pv << 8);
    *out++ = static_cast<uint8_t>((v >> 11) * 255 / 31);
    *out++ = static_cast<uint8_t>((v >> 5 & 63) * 255 / 63);
    *out++ = static_cast<uint8_t>((v & 31) * 255 / 31);
    *out++ = 255;
  }
}

}  // namespace

extern "C" {

// A cart from its source; false: it won't start (lw_error says why).
EMSCRIPTEN_KEEPALIVE int lw_start(const char* name, const char* src) {
  g_cart.reset();
  g_host.sounds.clear();
  g_pal_ver = UINT32_MAX;
  g_cart = std::make_unique<carts::Cart>(g_host);
  const bool ok = g_cart->start(name, src);
  g_error = g_cart->error();
  if (ok) compose();
  return ok ? 1 : 0;
}

// One frame with this input (held: a bit per button; tx, ty in cart pixels; ax, ay tilt -1..1).
// False once the cart has stopped (an error, or it ended): the last frame shows why.
EMSCRIPTEN_KEEPALIVE int lw_frame(int held, int touching, float tx, float ty, float ax, float ay) {
  if (!g_cart) return 0;
  carts::Cart& cart = *g_cart;
  if (carts::Hd* h = cart.hd()) h->release_front();
  carts::Input in;
  in.held = static_cast<uint8_t>(held);
  in.touching = touching != 0;
  in.tx = tx;
  in.ty = ty;
  in.ax = ax;
  in.ay = ay;
  const bool running = cart.frame(in);
  if (carts::Stage* st = cart.stage()) {
    st->prepare();
    st->publish();
  }
  if (carts::Hd* h = cart.hd()) h->publish();
  compose();
  g_error = cart.error();
  return running ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE const char* lw_error() { return g_error.c_str(); }
EMSCRIPTEN_KEEPALIVE int lw_fps() { return g_cart ? g_cart->fps() : 30; }
EMSCRIPTEN_KEEPALIVE int lw_full_canvas() { return g_cart && g_cart->hd() && g_cart->full_canvas() ? 1 : 0; }
EMSCRIPTEN_KEEPALIVE uint8_t* lw_pixels() { return g_rgba.data(); }
EMSCRIPTEN_KEEPALIVE int lw_width() { return kW; }
EMSCRIPTEN_KEEPALIVE int lw_height() { return kH; }

// The next sfx() the cart played, or -1.
EMSCRIPTEN_KEEPALIVE int lw_sound() {
  if (g_host.sounds.empty()) return -1;
  const int n = g_host.sounds.front();
  g_host.sounds.pop_front();
  return n;
}

// Built-in sound n as 16-bit PCM at `rate` (the same synth the board plays); lw_pcm_len samples.
EMSCRIPTEN_KEEPALIVE int16_t* lw_pcm(int n, int rate) {
  g_pcm = carts::sfx_render(n, rate);
  return g_pcm.data();
}
EMSCRIPTEN_KEEPALIVE int lw_pcm_len() { return static_cast<int>(g_pcm.size()); }

}  // extern "C"
