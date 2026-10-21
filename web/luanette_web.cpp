// Luanette in a browser: the engine built with Emscripten, hosted the way engine/host/cartrun.cpp
// hosts it, but live. The page (web/index.html) calls lw_start with a cart's source, then
// lw_frame at the cart's rate with the input it gathered (touch, keys, tilt); each frame is
// composed as the board composes it (scene, layer, the 3x picture with its scanline rows) into
// one RGBA image of the whole glass, which the page puts on a canvas.
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "cart.hpp"
#include "cartsrc.hpp"
#include "hd.hpp"
#include "sfx.hpp"
#include "stage.hpp"
#include "synth.hpp"

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
  void sound(std::shared_ptr<const carts::Sound> s) override;
  void music(std::shared_ptr<const carts::Song> song) override;
  void wake(double) override {}
  std::deque<std::string> logs;  // log() lines the page hasn't shown yet
  void log(std::string_view line) override {
    if (logs.size() < 500) logs.emplace_back(line);
  }
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
  int frame_budget() const override { return carts::kFrameBudget; }  // as check.py: a cart passes here, passes there
};

std::unique_ptr<carts::Synth> g_synth;  // the cart's own sounds and song, once the page has audio (lw_audio_start)
std::shared_ptr<const carts::Song> g_song;  // the cart's song, kept so it starts when audio does
std::vector<float> g_audio;

WebHost g_host;
void WebHost::sound(std::shared_ptr<const carts::Sound> s) {
  if (g_synth) g_synth->sfx(std::move(s));
}
void WebHost::music(std::shared_ptr<const carts::Song> song) {
  g_song = song;  // a browser allows no sound until a click: a song started before it plays from then
  if (g_synth) g_synth->music(std::move(song));
}
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
  g_host.logs.clear();
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
// The cart's 16 colors as it shows them now (palette() changes them), "rrggbb" each, spaces between;
// the starting 16 before a cart runs. The sprite editor and the color strip draw with them.
EMSCRIPTEN_KEEPALIVE const char* lw_colors() {
  static char out[16 * 7];
  for (int i = 0; i < 16; ++i) {
    const uint32_t rgb = g_cart ? g_cart->colors()[i] : carts::kPalette[i];
    std::snprintf(out + i * 7, 8, "%06x", static_cast<unsigned>(rgb & 0xFFFFFF));
    out[i * 7 + 6] = i < 15 ? ' ' : '\0';
  }
  return out;
}

// log() lines the page hasn't shown yet, and the next of them.
EMSCRIPTEN_KEEPALIVE int lw_log_count() { return static_cast<int>(g_host.logs.size()); }
EMSCRIPTEN_KEEPALIVE const char* lw_log() {
  static std::string line;
  if (g_host.logs.empty()) return "";
  line = std::move(g_host.logs.front());
  g_host.logs.pop_front();
  return line.c_str();
}

// The last frame's Lua instructions (by 100s, as check.py counts them).
EMSCRIPTEN_KEEPALIVE int lw_ops() { return g_cart ? g_cart->last().ops : 0; }
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
EMSCRIPTEN_KEEPALIVE int16_t* lw_pcm_data() { return g_pcm.data(); }  // the samples lw_render_audio made

// A cart's source as it ships (comments out, as push.py sends it), and the most the board takes.
EMSCRIPTEN_KEEPALIVE const char* lw_shipped(const char* src) {
  static std::string out;
  out = carts::shipped(src);
  return out.c_str();
}
EMSCRIPTEN_KEEPALIVE int lw_max_source() { return static_cast<int>(carts::kMaxSource); }

// ---- the cart's own sounds and song, and the sound editor's -------------------------------
// The page has audio at `rate` (its AudioContext's): a synth for the cart's sounds and song.
EMSCRIPTEN_KEEPALIVE void lw_audio_start(int rate) {
  g_synth = std::make_unique<carts::Synth>(rate);
  if (g_song) g_synth->music(g_song);
}
EMSCRIPTEN_KEEPALIVE int lw_audio_busy() { return g_synth && g_synth->busy() ? 1 : 0; }
// The next n samples, -1 to 1, for the page to queue.
EMSCRIPTEN_KEEPALIVE float* lw_audio_render(int n) {
  static std::vector<int32_t> acc;
  acc.assign(static_cast<size_t>(n), 0);
  g_audio.resize(static_cast<size_t>(n));
  if (g_synth) g_synth->render(acc.data(), n);
  for (int k = 0; k < n; ++k) g_audio[static_cast<size_t>(k)] = std::clamp(acc[static_cast<size_t>(k)], -32768, 32767) / 32768.0f;
  return g_audio.data();
}
EMSCRIPTEN_KEEPALIVE void lw_audio_stop() {
  if (!g_synth) return;
  g_synth->sfx(nullptr);
  g_synth->music(nullptr);
}

// The editor plays a sound's text: "" when it's fine (and plays), else what's wrong.
EMSCRIPTEN_KEEPALIVE const char* lw_play_sound(const char* text) {
  static std::string why;
  auto s = std::make_shared<carts::Sound>();
  why = carts::parse_sound(text, *s);
  if (why.empty() && g_synth) g_synth->sfx(std::move(s));
  return why.c_str();
}

// The editor's WAV export: a sound's text (song 0) or the song (song 1, JSON as lw_play_song, from
// pattern `start`) rendered offline on a synth of its own, for `seconds` at most or until it's done,
// at `rate`. The samples are 16-bit (lw_pcm_data, lw_pcm_len); "" when it's fine, else what's wrong.
EMSCRIPTEN_KEEPALIVE const char* lw_render_audio(const char* text, int song, int start, int rate, double seconds) {
  static std::string why;
  g_pcm.clear();
  carts::Synth synth(rate);
  if (song) {
    hg::json::Value v;
    if (!hg::json::parse(text, v) || !v.is_object()) {
      why = "not a song";
      return why.c_str();
    }
    auto lookup = [&v](const char* table) {
      return [&v, table](int i) -> const char* {
        const hg::json::Value& e = v[table][std::to_string(i)];
        return e.is_string() ? e.as_string().c_str() : nullptr;
      };
    };
    std::shared_ptr<const carts::Song> s;
    why = carts::build_song(start, lookup("music"), lookup("sounds"), s);
    if (!why.empty()) return why.c_str();
    synth.music(std::move(s));
  } else {
    auto s = std::make_shared<carts::Sound>();
    why = carts::parse_sound(text, *s);
    if (!why.empty()) return why.c_str();
    synth.sfx(std::move(s));
  }
  const size_t most = static_cast<size_t>(std::max(0.0, seconds) * rate);
  std::vector<int32_t> acc(1024);
  while (synth.busy() && g_pcm.size() < most) {
    const int n = static_cast<int>(std::min<size_t>(acc.size(), most - g_pcm.size()));
    std::fill(acc.begin(), acc.begin() + n, 0);
    synth.render(acc.data(), n);
    for (int k = 0; k < n; ++k) g_pcm.push_back(static_cast<int16_t>(std::clamp(acc[static_cast<size_t>(k)], -32768, 32767)));
  }
  {  // the end faded so it doesn't click: 50 ms where a loop was cut off, else 5 ms (a note may end mid-wave)
    const size_t fade = std::min(g_pcm.size(), static_cast<size_t>(synth.busy() ? rate / 20 : rate / 200));
    for (size_t k = 0; k < fade; ++k) {
      int16_t& x = g_pcm[g_pcm.size() - fade + k];
      x = static_cast<int16_t>(x * static_cast<float>(fade - k) / fade);
    }
  }
  return why.c_str();
}

// The editor plays its song from pattern `start`: {"music": {"0": "...", ...}, "sounds": {"8": "...", ...}}.
EMSCRIPTEN_KEEPALIVE const char* lw_play_song(const char* json, int start) {
  static std::string why;
  hg::json::Value v;
  if (!hg::json::parse(json, v) || !v.is_object()) {
    why = "not a song";
    return why.c_str();
  }
  auto text = [&v](const char* table) {
    return [&v, table](int i) -> const char* {
      const hg::json::Value& e = v[table][std::to_string(i)];
      return e.is_string() ? e.as_string().c_str() : nullptr;
    };
  };
  std::shared_ptr<const carts::Song> song;
  why = carts::build_song(start, text("music"), text("sounds"), song);
  if (why.empty() && g_synth) g_synth->music(std::move(song));
  return why.c_str();
}

}  // extern "C"
