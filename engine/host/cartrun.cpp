// Runs a cart with the board's runtime on the host, for tests: tools/check.py writes the frames'
// input (--dump-input), this replays it and writes the pictures, and check.py's own pictures are
// compared with them (tests/parity.py). The clock is the frame count, as in check.py.
//
//   cartrun CART.lua INPUT.json OUT_DIR [SHOTS]
//
// INPUT.json: {"fps": 30|60, "frames": [[held, tx|null, ty|null, ax, ay], ...], "images": path?,
// "live": path?, "data": {channel: value}?, "t0": seconds?, "updates": [[frame, channel, value]]?}
// (check.py's sample channels for data(), and its clock: now() is t0 plus the frames' time, as
// check.py counts it; "updates" change a channel before a frame, for tests of a cart's reactions)
// ("images": the cart's bundle from carts/pack_images.py, for hd.image; "live": the samples that
// stand in for live images, tools/samples/live/). Writes
// OUT_DIR/frame_<n>.bin (128*128 palette indices) for every frame, and prints one JSON line.
// A 3D cart (docs/api.md, "3D") is rendered as the board shows it, its scene through the 3D stage
// with the picture over it: OUT_DIR/frame_<n>.ppm (410x384, the glass's width) for the frames in SHOTS.

#include <signal.h>
#include <sys/time.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <set>
#include <vector>

#include "cart.hpp"
#include "hd.hpp"
#include "stage.hpp"

namespace {

carts::Cart* g_cart = nullptr;

// The deadline as on the board: an interruption of the Lua thread itself (here SIGALRM, as lua.c's Ctrl-C).
void on_alarm(int) {
  if (g_cart) g_cart->interrupt();
}

struct TestHost final : carts::Host {
  void deadline(int64_t us) override {
    itimerval t{};
    t.it_value.tv_sec = us / 1000000;
    t.it_value.tv_usec = us % 1000000;
    setitimer(ITIMER_REAL, &t, nullptr);
  }
  int64_t clock_us = 0;
  std::map<std::string, std::string> saves;
  int buzzes = 0, sfx_n = 0, acts = 0;
  int64_t now_us() override { return clock_us; }
  void* alloc(size_t n) override { return std::malloc(n); }
  void release(void* p) override { std::free(p); }
  void buzz(int) override { ++buzzes; }
  void sfx(int) override { ++sfx_n; }
  void sound(std::shared_ptr<const carts::Sound> s) override { sfx_n += s ? 1 : 0; }
  void wake(double) override {}
  std::vector<std::string> logs;  // log() lines, for the result's "log"
  void log(std::string_view line) override { if (logs.size() < 200) logs.emplace_back(line); }
  bool accepts_act(std::string_view) const override { return true; }  // check.py has the list; this replays its runs
  void act(std::string_view name, std::string_view arg) override {
    ++acts;
    if (std::getenv("HG_ACT_LOG"))  // each act() with its time, for tools (a demo's tap marks)
      std::fprintf(stderr, "act %lld %.*s %.*s\n", static_cast<long long>(clock_us), static_cast<int>(name.size()), name.data(),
                   static_cast<int>(arg.size()), arg.data());
  }
  hg::json::Value channels;  // check.py's samples ("data")
  std::map<std::string, uint32_t> versions;
  int64_t t0 = 0;            // check.py's wall clock at frame 0 ("t0"); 0: this machine's
  const hg::json::Value* data(std::string_view name, uint32_t* version) override {
    const hg::json::Value& v = channels[name];
    if (v.is_null()) return nullptr;
    if (version) {
      const auto it = versions.find(std::string(name));
      *version = it == versions.end() ? 1 : it->second;  // moves with each of the input's "updates"
    }
    return &v;
  }
  carts::Caps caps() override {
    carts::Caps c;  // the board's: cartrun stands in for it (previews, the parity test)
    c.board = "waveshare-amoled-206";
    c.corner = 100;
    return c;
  }
  int64_t unix_time() override { return t0 ? t0 + clock_us / 1000000 : static_cast<int64_t>(std::time(nullptr)); }
  std::string load_save(std::string_view c) override { return saves[std::string(c)]; }
  bool store_save(std::string_view c, std::string_view j) override {
    saves[std::string(c)] = std::string(j);
    return true;
  }
  std::string bundle;  // the cart's images (pack_images.py), as the board keeps them
  std::vector<carts::HdBundleEntry> index;
  std::string live;  // stand-ins for the host's live images
  std::vector<carts::HdBundleEntry> live_index;
  static bool find(const std::string& b, const std::vector<carts::HdBundleEntry>& idx, std::string_view name, std::vector<uint8_t>& out) {
    for (const auto& e : idx)
      if (e.name == name && static_cast<size_t>(e.off) + e.len <= b.size()) {
        out.assign(b.begin() + e.off, b.begin() + e.off + e.len);
        return true;
      }
    return false;
  }
  bool image(std::string_view name, std::vector<uint8_t>& out) override {
    return find(bundle, index, name, out) || find(live, live_index, name, out);
  }
  std::vector<std::string> image_names() override {
    std::vector<std::string> out;
    for (const auto& e : index) out.push_back(e.name);
    return out;
  }
};

std::string read(const char* path) {
  std::ifstream f(path, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: cartrun CART.lua INPUT.json OUT_DIR\n");
    return 2;
  }
  const std::string src = read(argv[1]);
  hg::json::Value in;
  std::string err;
  if (!hg::json::parse(read(argv[2]), in, &err)) {
    std::fprintf(stderr, "input: %s\n", err.c_str());
    return 2;
  }
  std::set<int> shots;
  if (argc > 4)
    for (const char* p = argv[4]; *p;) {
      shots.insert(std::atoi(p));
      while (*p && *p != ',') ++p;
      if (*p) ++p;
    }
  // The picture's colors over a scene (RGB565, the panel's byte order as the stage draws), and
  // its scanline rows a quarter darker: from the cart's palette, made again when it changes it.
  uint16_t pal[16], dim[16];
  uint32_t pal_ver = UINT32_MAX;
  auto set_colors = [&](const carts::Cart& c) {
    auto pack = [](uint32_t r, uint32_t g, uint32_t b) {
      const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
      return static_cast<uint16_t>(v >> 8 | v << 8);
    };
    for (int i = 0; i < 16; ++i) {
      const uint32_t rgb = c.colors()[i];
      pal[i] = pack(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255);
      dim[i] = pack((rgb >> 16 & 255) * 3 / 4, (rgb >> 8 & 255) * 3 / 4, (rgb & 255) * 3 / 4);
    }
    pal_ver = c.colors_version();
  };
  bool any3d = false, anyhd = false;
  TestHost host;
  if (in["images"].is_string()) {
    host.bundle = read(std::string(in["images"].str_or("")).c_str());
    if (carts::hd_bundle_index(reinterpret_cast<const uint8_t*>(host.bundle.data()), host.bundle.size(), host.index) != 0) {
      std::fprintf(stderr, "images: %s isn't a bundle (carts/pack_images.py)\n", std::string(in["images"].str_or("")).c_str());
      return 2;
    }
  }
  if (in["live"].is_string()) {
    host.live = read(std::string(in["live"].str_or("")).c_str());
    if (carts::hd_bundle_index(reinterpret_cast<const uint8_t*>(host.live.data()), host.live.size(), host.live_index) != 0) {
      std::fprintf(stderr, "live: %s isn't a bundle (carts/pack_images.py)\n", std::string(in["live"].str_or("")).c_str());
      return 2;
    }
  }
  host.channels = in["data"];
  host.t0 = in["t0"].as_int(0);
  carts::Cart cart(host);
  g_cart = &cart;
  signal(SIGALRM, on_alarm);
  hg::json::Value res = hg::json::Value::object();
  const bool ok = cart.start("test", src);
  const int fps = cart.fps();
  double max_us = 0, total_us = 0;
  int rate = fps, base_n = 0;  // the clock: frame n's time is base_us + (n - base_n) frames at `rate`
  int64_t base_us = 0;
  const bool verify_hd = std::getenv("HG_HD_VERIFY") != nullptr, hold_front = std::getenv("HG_HD_HOLD") != nullptr;
  hg::json::Value hd_bad = hg::json::Value::array();  // frame * 1000 + the first bad row
  int n = 0;
  if (ok) {
    for (const auto& f : in["frames"].elements()) {
      ++n;
      if (cart.fps() != rate) {  // the cart changed its rate (fps()): the clock goes on from the last frame's time
        base_us = host.clock_us;
        base_n = n - 1;
        rate = cart.fps();
      }
      host.clock_us = base_us + static_cast<int64_t>(n - base_n) * 1000000 / rate;
      if (std::getenv("HG_FRAME_LOG")) std::fprintf(stderr, "frame %d %lld\n", n, static_cast<long long>(host.clock_us));  // each frame's time, for tools
      for (const auto& u : in["updates"].elements()) {  // a channel changing before this frame
        if (u[0].as_int() != n) continue;
        host.channels.set(u[1].as_string(), u[2]);
        host.versions[u[1].as_string()] = (host.versions.count(u[1].as_string()) ? host.versions[u[1].as_string()] : 1) + 1;
      }
      carts::Input ci;
      ci.held = static_cast<uint8_t>(f[0].as_int());
      ci.touching = f[1].is_number();
      ci.tx = static_cast<float>(f[1].as_number());
      ci.ty = static_cast<float>(f[2].as_number());
      ci.ax = static_cast<float>(f[3].as_number());
      ci.ay = static_cast<float>(f[4].as_number());
      // The last frame's sending (here, its shots) is done: as the board's sender, the front is let
      // go, so the cart draws on shared rows in place. HG_HD_HOLD keeps it held (every frame as one
      // that starts while the last still goes out: copy on write), to test that way too.
      if (carts::Hd* h = cart.hd(); h && !hold_front) h->release_front();
      const auto t0 = std::chrono::steady_clock::now();
      const bool fr = cart.frame(ci);
      const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
      max_us = std::max(max_us, us);
      if (std::getenv("HG_LUA_LOG")) std::fprintf(stderr, "lua %d %.1f\n", n, us);  // each frame's Lua time (this machine's), for tools
      total_us += us;
      char path[512];
      std::snprintf(path, sizeof path, "%s/frame_%d.bin", argv[3], n);
      std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(cart.picture()), carts::kW * carts::kH);
      carts::Stage* st = cart.stage();
      if (st) {  // as the board: prepared after the frame, then drawn band by band
        any3d = true;
        st->prepare();
        st->publish();
      }
      carts::Hd* hd = cart.hd();
      if (hd) {
        anyhd = true;
        hd->publish();  // the rows it changed go to the front buffer, which is what's composed
        if (verify_hd) {  // HG_HD_VERIFY: the front buffer checked against the back after every frame
          const int bad = hd->verify();
          if (bad >= 0 && hd_bad.elements().size() < 20) hd_bad.push(hg::json::Value(static_cast<double>(n * 1000 + bad)));
        }
      }
      if ((st || hd || std::getenv("HG_SHOT_ALL")) && shots.count(n)) {  // (HG_SHOT_ALL: a picture-only cart too, for tools)
        // As the board composes a band: the scene (or the picture's color 0), the layer if under,
        // the 16-color picture (color 0 see-through), the layer if over; scanline rows a quarter darker.
        // A full canvas ("-- canvas: full"): the whole glass, its rows around the window the layer
        // over color 0 (as the board's edge rows); else the window's rows.
        const int W = carts::kStageW, H = carts::Stage::kH;
        const bool full = hd && cart.full_canvas();
        const carts::Caps caps = host.caps();
        const int top = full ? caps.window_y : 0, OH = full ? caps.glass_h : H;
        std::vector<uint16_t> img(static_cast<size_t>(W) * OH);
        std::vector<uint16_t> work(st ? static_cast<size_t>(st->work_cells()) : 0);
        if (cart.colors_version() != pal_ver) set_colors(cart);
        const int band = st ? st->band_rows() : 16;
        for (int y = 0; y < H; y += band) {
          uint16_t* rows = img.data() + static_cast<size_t>(top + y) * W;
          if (st) {
            st->band(y, y + band, rows, work.data());
          } else {
            for (int r = 0; r < band; ++r) std::fill(rows + r * W, rows + (r + 1) * W, (y + r) % 3 == 2 ? dim[0] : pal[0]);
          }
          auto layer = [&]() {
            for (int r = 0; r < band; ++r)
              hd->compose(rows + r * W, top + y + r, full ? 0 : -carts::kStageX, W);
          };
          if (hd && !hd->over()) layer();
          carts::overlay_rows(rows, W, carts::kStageX, y, band, cart.picture(), pal, dim);
          if (hd && hd->over()) layer();
        }
        for (int gy = 0; full && gy < OH; ++gy) {
          if (gy >= top && gy < top + H) continue;
          uint16_t* row = img.data() + static_cast<size_t>(gy) * W;
          std::fill(row, row + W, gy % 3 == 2 ? dim[0] : pal[0]);
          hd->compose(row, gy, 0, W);
        }
        std::snprintf(path, sizeof path, "%s/frame_%d.ppm", argv[3], n);
        std::ofstream o(path, std::ios::binary);
        o << "P6\n" << W << " " << OH << "\n255\n";
        for (const uint16_t pv : img) {
          const uint16_t v = static_cast<uint16_t>(pv >> 8 | pv << 8);
          const char px[3] = {static_cast<char>((v >> 11) * 255 / 31), static_cast<char>((v >> 5 & 63) * 255 / 63), static_cast<char>((v & 31) * 255 / 31)};
          o.write(px, 3);
        }
      }
      if (!fr) break;
    }
  }
  res.set("ok", cart.error().empty()).set("error", cart.error()).set("fps", fps).set("frames", n)
      .set("max_us", max_us).set("avg_us", n ? total_us / n : 0.0)
      .set("memory", static_cast<double>(cart.memory())).set("buzz", host.buzzes).set("sfx", host.sfx_n).set("stage", any3d).set("hd", anyhd).set("hd_bad", hd_bad)
      .set("triangles", cart.stage() ? cart.stage()->triangles() : 0).set("queued", cart.stage() ? cart.stage()->queued() : 0);
  hg::json::Value log = hg::json::Value::array();
  for (const std::string& line : host.logs) log.push(hg::json::Value(line));
  res.set("log", std::move(log));
  std::printf("%s\n", res.dump().c_str());
  return cart.error().empty() ? 0 : 1;
}
