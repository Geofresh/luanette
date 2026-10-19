#include "cart_player.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "cart.hpp"
#include "cart_store.hpp"
#include "driver/gptimer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hd.hpp"
#include "hg/panel_pixels.hpp"
#include "nvs.h"
#include "perfmon.h"
#include "scan_sync.hpp"
#include "sound.hpp"
#include "stage.hpp"

namespace carts::player {

Tuning tuning;

namespace {
using namespace carts::scan;
namespace cartstore = carts::store::cartstore;
namespace imagestore = carts::store::imagestore;
namespace live_images = carts::store::live_images;

const char* kTag = "hg.carts";
constexpr int64_t kStatsUs = 5'000'000;  // the stats lines, every 5 s while a cart runs

Device* dev = nullptr;
carts::store::Library* g_lib = nullptr;
Config g_cfg;
struct CartRun;
CartRun* g_cart = nullptr;         // the open cart (the ticking task's; others read it under the host's lock)
std::string g_open_name;           // its name
std::atomic<bool> g_slow{false};   // a frame every 100 ms or more
std::atomic<bool> g_gesture_closed{false};
std::atomic<bool> g_button_b{false};
// The tuning aids, under their old names
std::atomic<int>& cart_profile_n = tuning.profile_frames;
std::atomic<int>& cart_profile_core = tuning.profile_core;
std::atomic<int>& cart_panel_check = tuning.panel_check;
std::atomic<int>& cart_perf_n = tuning.perf_frames;
int (&g_tilt)[4] = tuning.tilt;

// A cart (cart.hpp: a small Lua program) takes the screen. The host's own screen steps aside
// (Device::will_open: nothing of it runs or draws), and the
// cart runs a frame at the start of each panel pass (every other pass for a 30 fps cart) on this
// task, core 1, into a 128x128 picture of palette indices. A sender on core 0 sends the rows
// that changed, 3x and with dim scanlines, through the band buffers, each band just
// behind the panel's scan (te_gate): frames land whole, never torn. No buttons on screen: carts
// use touch and tilt; for those that read buttons, a tap is A, a drag is the d-pad (from where the
// finger came down) and BOOT is B. PWR, or a swipe down from the top edge, leaves.
// No cart open: no task, no memory, one branch in the host's loop.
constexpr int kCartScale = 3, kCartPx = carts::kW * kCartScale;  // 384
// Centred; y even (the controller's rule) and a multiple of 3, so each pixel's third row is the dim
// scanline (rows y % 3 == 2).
constexpr int kCartX = 12, kCartY = 60;
constexpr int kGlassRows = 502;  // the AMOLED-2.06's glass (a full canvas's height)
constexpr int kFullCanvasExitStrip = 24;  // a full canvas: a swipe down from the glass's top 24 rows leaves
constexpr int kTapMaxUs = 350000, kDragPx = 18;  // a tap: up again this soon without moving this far
constexpr int kBandCartRows = 10;  // 30 panel rows a band (the band buffers hold 32 rows of 410)
constexpr int64_t kCartRacePrepUs = 1800;      // racing: the handoff and the first band's conversion, and a margin
constexpr int64_t kStageRaceMarginUs = 4000;   // a stage's (see cart_tick): its frames start this far ahead of their need
constexpr int64_t kStageStartMarginUs = 1200;  // and race only with this much of it left when they do start
constexpr int64_t kStageBandSendUs = 1300;     // a band's transfer and te_gate's guard, in a stage frame's needed lead
constexpr int64_t kCartRaceMaxLeadUs = 14000;  // a frame that needs a longer lead goes behind the scan



class CartHost final : public carts::Host {
 public:
  explicit CartHost(Device& d) : dev_(d) {}
  esp_timer_handle_t wake_timer = nullptr;
  gptimer_handle_t deadline_timer = nullptr;  // its interrupt on core 1, where Lua runs (made there)
  bool deadline_on = false;  // enabled: only while a frame runs (an enabled timer holds a power lock
                             // that forbids light sleep; enabled all the time, a dark board never slept)
  carts::Cart* cart = nullptr;
  void deadline(int64_t us) override {
    if (!deadline_timer) return;
    if (deadline_on) {
      gptimer_stop(deadline_timer);
      gptimer_disable(deadline_timer);
      deadline_on = false;
    }
    if (us <= 0) return;
    if (gptimer_enable(deadline_timer) != ESP_OK) return;
    deadline_on = true;
    gptimer_set_raw_count(deadline_timer, 0);
    gptimer_alarm_config_t ac = {};
    ac.alarm_count = static_cast<uint64_t>(us);
    gptimer_set_alarm_action(deadline_timer, &ac);
    gptimer_start(deadline_timer);
  }
  int64_t now_us() override { return esp_timer_get_time(); }
  // 16-byte aligned: the layer's planes are read by the vector loops (hd_pie.S) a line at a time.
  void* alloc(size_t n) override { return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }
  void release(void* p) override { heap_caps_free(p); }
  void* alloc_fast(size_t n) override {
    void* p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return p ? p : alloc(n);
  }
  void release_fast(void* p) override { heap_caps_free(p); }
  // The open cart's images: its bundle mapped into the address space at open (read through the
  // flash cache: an esp_partition_read into PSRAM would bounce through up to 16 KB of internal RAM),
  // unmapped at close.
  imagestore::Bundle images;
  std::vector<carts::HdBundleEntry> image_index;
  const uint8_t* image_map = nullptr;
  esp_partition_mmap_handle_t image_map_handle = 0;
  void load_images(const std::string& name) {
    image_index.clear();
    if (!imagestore::find(name, images)) return;
    const void* p = nullptr;
    const esp_err_t err = esp_partition_mmap(cartstore::part(), images.data(), images.len, ESP_PARTITION_MMAP_DATA, &p, &image_map_handle);
    if (err != ESP_OK) {
      ESP_LOGW(kTag, "cart %s: its images can't be mapped (%s); none shown", name.c_str(), esp_err_to_name(err));
      return;
    }
    image_map = static_cast<const uint8_t*>(p);
    imagestore::note_used(name);
    if (carts::hd_bundle_index(image_map, images.len, image_index) != 0) {
      image_index.clear();
      ESP_LOGW(kTag, "cart %s: its images' index is damaged; none shown", name.c_str());
      return;
    }
    ESP_LOGI(kTag, "cart %s: %u images (%u KB)", name.c_str(), static_cast<unsigned>(image_index.size()), static_cast<unsigned>(images.len / 1024));
  }
  ~CartHost() {
    if (image_map) esp_partition_munmap(image_map_handle);
  }
  bool image(std::string_view name, std::vector<uint8_t>& out) override {
    for (const auto& e : image_index) {
      if (e.name != name) continue;
      if (static_cast<size_t>(e.off) + e.len > images.len) return false;
      out.assign(image_map + e.off, image_map + e.off + e.len);
      return true;
    }
    return live_images::get(name, out);  // not the cart's own: one from the host
  }
  std::vector<std::string> image_names() override {
    std::vector<std::string> out;
    for (const auto& e : image_index) out.push_back(e.name);
    return out;
  }
  carts::Caps caps() override {
    carts::Caps c;  // a 384 x 384 square window at one panel pixel a canvas unit
    c.tilt = dev_.has_imu();
    c.buzz = dev_.has_motor();
    c.sound = carts::sound::available();
    c.board = "waveshare-amoled-206";
    c.window_x = kCartX;
    c.window_y = kCartY;
    c.glass_w = dev_.width();
    c.glass_h = dev_.height();
    c.corner = dev_.corner_radius();  // the board profile's (measured)
    c.round = dev_.round();
    return c;
  }
  void buzz(int ms) override {
    if (dev_.has_motor()) dev_.buzz(ms);
    else if (!buzz_noted_) ESP_LOGW(kTag, "cart: buzz(%d) asked for; this board has no vibration motor", ms);
    buzz_noted_ = true;
  }
  void sfx(int n) override { carts::sound::play(n); }  // (esp32/sound.hpp; silent without a speaker)
  bool level_due = false;  // the cart asked for level(): the next reading sets the table
  void level() override { level_due = true; }
  void wake(double secs) override {
    if (!wake_timer) return;
    esp_timer_stop(wake_timer);
    if (secs >= 0) esp_timer_start_once(wake_timer, static_cast<uint64_t>(secs * 1e6));
  }
  void act(std::string_view name, std::string_view arg) override { dev_.command(name, arg); }
  const hg::json::Value* data(std::string_view name, uint32_t* version) override { return dev_.data(name, version); }
  // Saves in NVS ("hgcarts", a key from the cart's name). A flash write pauses both cores for
  // a few ms: a save costs a frame, which is why carts save at moments, not every frame.
  std::string load_save(std::string_view cart) override {
    nvs_handle_t h;
    std::string out;
    if (nvs_open("hgcarts", NVS_READONLY, &h) != ESP_OK) return out;
    size_t len = 0;
    const std::string k = key(cart);
    if (nvs_get_str(h, k.c_str(), nullptr, &len) == ESP_OK && len > 1) {
      out.resize(len);
      if (nvs_get_str(h, k.c_str(), out.data(), &len) == ESP_OK) out.resize(len - 1);
      else out.clear();
    }
    nvs_close(h);
    return out;
  }
  bool store_save(std::string_view cart, std::string_view json) override {
    nvs_handle_t h;
    esp_err_t err = nvs_open("hgcarts", NVS_READWRITE, &h);
    if (err == ESP_OK) {
      err = nvs_set_str(h, key(cart).c_str(), std::string(json).c_str());
      if (err == ESP_OK) err = nvs_commit(h);
      nvs_close(h);
    }
    if (err != ESP_OK) ESP_LOGE(kTag, "cart %.*s: save failed: %s", static_cast<int>(cart.size()), cart.data(), esp_err_to_name(err));
    return err == ESP_OK;
  }

 private:
  static std::string key(std::string_view cart) {  // NVS keys are 15 characters at most
    uint32_t h = 2166136261u;
    for (const char ch : cart) h = (h ^ static_cast<uint8_t>(ch)) * 16777619u;
    char k[12];
    std::snprintf(k, sizeof k, "s%08lx", static_cast<unsigned long>(h));
    return k;
  }
  Device& dev_;
  bool buzz_noted_ = false;
};

struct CartRun {
  explicit CartRun(Device& d) : host(d) {}
  CartHost host;
  carts::Cart cart{host};
  uint8_t* tx = nullptr;     // the frame the sender is sending (PSRAM)
  uint8_t* shown = nullptr;  // what the panel shows (PSRAM); 0xFF where unknown
  uint16_t pal[16], dim[16];  // panel byte order; dim: the scanline rows, a quarter darker
  uint32_t pal_ver = UINT32_MAX;  // the cart's colours_version() they were made from
  carts::Hd* hd = nullptr;        // the cart's full-colour layer, as of the last handoff (the sender's to compose)
  bool full = false;              // its layer covers the glass ("-- canvas: full"): canvas = glass pixels
  uint32_t edge_due[(kGlassRows + 31) / 32] = {};  // a full canvas's rows outside the window still to send
  // The glass columns [span0, span1) of a row due only because the layer changed there (span1 0: all
  // of it): an edge row's, and a picture row's (layer_due: the layer changed under or over it).
  uint16_t edge_span0[kGlassRows] = {}, edge_span1[kGlassRows] = {};
  uint32_t layer_due[(carts::kH + 31) / 32] = {};
  uint16_t pic_span0[carts::kH] = {}, pic_span1[carts::kH] = {};
  // {"panel_check"}: a copy of the glass as the sender wrote it (PSRAM, while checking), the frames
  // left to check, and the mismatches found.
  uint16_t* shadow = nullptr;
  int check_left = 0, check_frames = 0, check_bad = 0, check_x = -1, check_y = -1;
  TaskHandle_t sender = nullptr;
  std::atomic<bool> busy{false}, quit{false}, alive{false};
  int64_t next_pass = -1, last_due = 0;  // the pass the next frame shows on
  // Racing the beam (te.ahead): a frame starts `lead` before the pass it shows on and its bands go
  // just ahead of that pass's scan: input to glass a pass sooner than behind the scan. The lead is
  // the recent worst Lua time plus the first band's preparation and a margin.
  int64_t lua_recent[16] = {};
  int lua_i = 0;
  int64_t tx_race_pass = -1, tx_age = -1, tx_t0 = 0;  // for the sender: this frame's pass to race (-1: behind), its sample's age
  std::atomic<uint32_t> raced{0}, fell_back{0};
  bool finger = false, edge = false;  // edge: the swipe from the top edge that leaves
  bool boot_was = false;              // BOOT was down at the last frame (a lazy cart wakes on its press)
  bool poked = false;                 // a lazy cart's frame was brought forward for input, not yet run
  int down_x = 0, down_y = 0;
  int64_t down_us = 0;
  bool moved = false;                 // a drag, not a tap
  bool tap = false;                   // A, for the frame after a tap
  // Tilt is measured against how the board was held when the cart started (or at level()): a
  // virtual table, level in that pose. up0: gravity's reaction then, in chip axes; r, d: the
  // screen's right and down laid flat on that table.
  float acc[3] = {};                  // smoothed reading
  float r[3] = {}, d[3] = {};
  bool table = false, relevel = true;
  int readings = 0;  // since the accelerometer came on: its first samples aren't settled
  float tilt_x = 0, tilt_y = 0;  // the last tilt() given, for the stats line
  bool imu_on = false;
  // Stats (the ticking task's, and the sender's sums)
  uint32_t frames = 0, late = 0;
  int64_t lua_us = 0, lua_max = 0, window_t0 = 0;
  size_t lua_max_freed = 0;  // what the Lua heap gave back in the slowest frame (a collection ran in it: tens of KB)
  int64_t lua_own_max = 0;   // a 3D cart's slowest Lua frame less the helper's drawing in it
  int64_t hd_us = 0, hd_max = 0;  // the full-colour layer's publish() at each handoff
  std::atomic<uint32_t> rows_sent{0}, tx_frames{0};
  mutable std::atomic<uint32_t> px_sent{0}, px_rows{0};  // pixels to the panel (cart_write), and in the rows they're in
  std::atomic<int64_t> tx_us{0}, tx_max{0};
  std::atomic<int64_t> tx_wait_us{0};   // of tx_us, waiting for the scan (te_gate)
  std::atomic<int64_t> tx_phase_us{0};  // where in the panel's pass each frame was handed over (sum)
  int prof_left = 0;  // frames still to sample ({"cart_profile": n})
  int perf_frame = 0;  // frames counted so far ({"cart_perf": n})
  uint32_t dropped = 0;  // frames the sender couldn't take (the stats line)
  // A 3D stage instead of a Lua cart ("@stage", the Jet test): drawn at full resolution by the
  // sender, band by band, each band just before it goes out; depth: its band of depth cells.
  carts::Stage* stage = nullptr;              // the cart's (a 3D cart: docs/api.md, "3D") or the test's
  std::unique_ptr<carts::Stage> own_stage;     // the test scene's ("@stage")
  bool overlay = false;                        // a 3D cart: its picture goes over the scene (colour 0: clear)
  int64_t prep_us = 0, prep_max = 0;           // the stage's preparing (core 1; sums for the stats)
  uint16_t* depth = nullptr;  // internal RAM, kStageBand rows of the stage's width
  std::string stage_name;
  int64_t stage_t0 = 0;
  std::atomic<int64_t> raster_us{0}, raster_max{0};  // the sender's drawing, a frame (sums for the stats)
  std::atomic<uint32_t> raster_tris{0};
  // Racing a stage: the lead its last frame needed, from the sender (each band drawn by its
  // deadline: the scan reaching its top row); -1 once taken.
  std::atomic<int64_t> need_lead{-1};
  int seam_band = -1;                      // the last seam's band, the time it had left before the scan, its drawing
  int64_t seam_slack = 0, seam_draw = 0;
  float drag_x = 0, drag_y = 0, hold_x = 0, hold_y = 0;  // the stage's turn by finger: now, and as last let go
  int64_t start_late_us = 0, start_late_max = 0;  // a stage frame's start after its due time (sums for the stats)
  uint32_t te_late0 = 0, te_bands0 = 0, seams0 = 0;  // te's counts at the last stats line
  int64_t band_draw[12] = {}, band_ready[12] = {}, band_out[12] = {};  // per band: drawing, drawn by, sent by (from the frame's start; sums)
  // The stage's second core: a helper on core 1 draws each band's right half while the sender
  // draws its left (they share the band buffer and the work cells: different columns).
  TaskHandle_t helper = nullptr;
  SemaphoreHandle_t help_done = nullptr;
  std::atomic<bool> helper_alive{false}, help_quit{false};  // help_quit: set once the sender has stopped
  int help_y = 0, help_k = 0, help_x = 0;  // the band to help with, and its columns [help_x, kW) (set before the notify)
  std::atomic<int64_t> help_us{0};  // its drawing (sum, for the stats)
  std::atomic<int64_t> help_total{0};  // its drawing, all told (a Lua frame's share is the helper's, not Lua's)
  // Where the band splits between the cores: halves, unless core 1 (which also prepares the next
  // frame) would overrun; then core 0 takes more columns.
  std::atomic<int> split{carts::Stage::kW / 2};  // the sender's: [0, split); a multiple of 4
  int stage_x = kCartX;                           // where the stage's columns start on the glass (0: full width)
  float share0 = 0.5f;                       // core 0's share of the columns (smoothed)
  int64_t help_frame = 0, own_frame = 0;     // the last frame's drawing: the helper's, the sender's own
  int64_t raster_frame = 0;                  // the sender's whole work on the last frame's bands (the scene, the picture, the layer)
  float res_load = 0;                        // res("auto"): a frame's work at full size, smoothed (us)
  int64_t res_switched = 0;                  // when it last changed the scale
  std::atomic<int64_t> help_frame_acc{0};    // the helper's, accumulating over the frame
  std::atomic<int> split_avg_acc{0};         // for the stats: the split, summed over frames
};

uint16_t to_panel(uint32_t rgb) {
  const uint32_t r = (rgb >> 16) & 255, gr = (rgb >> 8) & 255, b = rgb & 255;
  const uint16_t v = static_cast<uint16_t>((r >> 3) << 11 | (gr >> 2) << 5 | b >> 3);
  return static_cast<uint16_t>(v << 8 | v >> 8);  // the panel takes big-endian
}

// The cart's 16 colours as the sender draws them: panel order, and each a quarter darker for the
// scanline rows.
void set_colours(CartRun& c) {
  const uint32_t* rgb = c.cart.colours();
  for (int i = 0; i < 16; ++i) {
    c.pal[i] = to_panel(rgb[i]);
    c.dim[i] = hg::panel_order(hg::scan_dim(hg::panel_order(c.pal[i])));
  }
  c.pal_ver = c.cart.colours_version();
}

// One picture row (n palette indices) as three panel rows (two bright, one dim) of `stride`
// pixels: the picture 3x from column x0, and its edge pixels' colours out to the rows' ends. A
// picture that stopped at the window met the black margin in a hard vertical edge, which this
// AMOLED's subpixel layout draws as a coloured zig-zag; full-width rows never have one.
void expand_row(uint16_t* d, int stride, int x0, const uint8_t* s, int n, const uint16_t* pal, const uint16_t* dim) {
  const int w = n * kCartScale;
  auto row = [&](uint16_t* out, const uint16_t* p) {
    auto* a = reinterpret_cast<uint32_t*>(out + x0);  // two source pixels: six out, three words (x0 even)
    for (int x = 0; x < n; x += 2, a += 3) {
      const uint32_t p0 = p[s[x] & 15], p1 = p[s[x + 1] & 15];
      a[0] = p0 | p0 << 16;
      a[1] = p0 | p1 << 16;
      a[2] = p1 | p1 << 16;
    }
    const uint16_t l = p[s[0] & 15], r = p[s[n - 1] & 15];
    for (int x = 0; x < x0; ++x) out[x] = l;
    for (int x = x0 + w; x < stride; ++x) out[x] = r;
  };
  row(d, pal);
  std::memcpy(d + stride, d, static_cast<size_t>(stride) * 2);
  row(d + 2 * stride, dim);
}

// The full-colour layer's canvas rows [y, y + h) over `out` (h rows of `stride`, the canvas's
// column 0 at x), its edge columns out to the glass.
void layer_rows(const carts::Hd& hd, uint16_t* out, int stride, int x, int y, int h) {
  for (int i = 0; i < h; ++i)
    hd.compose(out + static_cast<size_t>(i) * stride, y + i, -x, stride);
}

// A canvas row straight from the layer's front row, baked as the panel takes it on the way: for a
// row that's opaque all along, where the picture adds nothing. Its edge columns run out to the
// glass as compose() would carry them (a full canvas has none: it is the glass).
inline void layer_row_direct(const carts::Hd& hd, uint16_t* out, int stride, int x, int y) {
  const int w = std::min(hd.width(), stride - x);
  if (w == hd.width()) {
    hd.front_out(out + x, y);
  } else {
    hd.compose(out + x, y, 0, w);  // (narrower than the canvas: as composed, which bakes the same)
  }
  std::fill_n(out, x, out[x]);
  std::fill_n(out + x + w, stride - x - w, out[x + w - 1]);
}

// The picture's row r (n wide) all colour 0: nothing of it shows over the layer.
inline bool pic_row_clear(const uint8_t* pic, int n, int r) {
  const uint8_t* p = pic + static_cast<size_t>(r) * n;
  for (int i = 0; i < n; i += 4) {
    uint32_t w;
    std::memcpy(&w, p + i, 4);
    if (w & 0x0F0F0F0F) return false;
  }
  return true;
}

// Picture row r of an n-wide picture as its three panel rows (`stride` wide, the picture's column
// 0 at x), with the cart's full-colour layer: what the sender sends and a screenshot shows.
void cart_row(uint16_t* d, int stride, int x, const uint8_t* pic, int n, int r, const CartRun& c) {
  const int py = r * kCartScale;  // its three rows, from the picture's top
  const carts::Hd* hd = n == carts::kW ? c.hd : nullptr;
  // The layer's canvas: the window (its row 0 the picture's, its column 0 at x), or the glass.
  const int cy = py + (c.full ? kCartY : 0), lx = c.full ? x - kCartX : x;
  // The layer opaque over all three, and the picture under it or clear there: the rows as they are.
  if (hd && hd->row_opaque(cy) && hd->row_opaque(cy + 1) && hd->row_opaque(cy + 2) && (hd->over() || pic_row_clear(pic, n, r))) {
    for (int i = 0; i < kCartScale; ++i) layer_row_direct(*hd, d + static_cast<size_t>(i) * stride, stride, lx, cy + i);
  } else if (hd && !hd->over()) {  // under: colour 0's background, the layer, then the picture's other colours
    for (int i = 0; i < kCartScale; ++i) std::fill_n(d + static_cast<size_t>(i) * stride, stride, i == 2 ? c.dim[0] : c.pal[0]);
    layer_rows(*hd, d, stride, lx, cy, kCartScale);
    carts::overlay_rows(d, stride, x, py, kCartScale, pic, c.pal, c.dim);
  } else {
    expand_row(d, stride, x, pic + static_cast<size_t>(r) * n, n, c.pal, c.dim);
    if (hd) layer_rows(*hd, d, stride, lx, cy, kCartScale);
  }
}

// A full canvas's glass row y outside the window (no picture there): the layer over colour 0.
void edge_row(uint16_t* d, int stride, int y, const CartRun& c) {
  if (c.hd->row_opaque(y)) {
    layer_row_direct(*c.hd, d, stride, 0, y);
    return;
  }
  std::fill_n(d, stride, y % 3 == 2 ? c.dim[0] : c.pal[0]);
  c.hd->compose(d, y, 0, stride);
}

// Rows of w packed pixels to the glass at (x0, y0), out of band buffer k (and into the check's copy
// of the glass, while there's one).
bool cart_write(const CartRun& c, int x0, int y0, int w, int h, uint16_t* b, int k) {
  c.px_sent += static_cast<uint32_t>(w * h);
  c.px_rows += static_cast<uint32_t>(dev->width() * h);
  if (c.shadow)
    for (int i = 0; i < h; ++i)
      std::memcpy(c.shadow + static_cast<size_t>(y0 + i) * dev->width() + x0, b + static_cast<size_t>(i) * w, static_cast<size_t>(w) * 2);
  (void)b;  // band k's own buffer
  return dev->band_send(k, x0, y0, w, h);
}

// A row's pending columns widened by [x0, x1) (glass columns); due: whether it was pending already
// (pending with span1 0: all of it, which stays so).
inline void widen(uint16_t& s0, uint16_t& s1, bool due, int x0, int x1) {
  if (!due) {
    s0 = static_cast<uint16_t>(x0), s1 = static_cast<uint16_t>(x1);
  } else if (s1) {
    s0 = static_cast<uint16_t>(std::min<int>(s0, x0)), s1 = static_cast<uint16_t>(std::max<int>(s1, x1));
  }
}

// A band composed at the glass's width in b (h rows of `stride`), reduced to its columns [x0, x1)
// packed row after row at the buffer's start (only they go to the panel: a narrow change, a narrow
// transfer). Even columns either side, as the controller wants.
// Each row moves left (or stays), so a forward copy is safe; the columns are even, so every row
// starts on a 4-byte boundary and moves in words. (memmove did this byte by byte on the S3, the
// rows overlapping: 9.3 of a 16 ms send for a cart redrawing most of the window, 2026-10-19.)
int pack_columns(uint16_t* b, int stride, int h, int& x0, int x1) {
  x0 &= ~1;
  x1 = std::min(stride, (x1 + 1) & ~1);
  const int w = x1 - x0;
  if (w < stride) {
    const int words = w / 2;
    for (int i = 0; i < h; ++i) {
      auto* d = reinterpret_cast<uint32_t*>(b + static_cast<size_t>(i) * w);
      const auto* s = reinterpret_cast<const uint32_t*>(b + static_cast<size_t>(i) * stride + x0);
      if (d == s) continue;
      int k = 0;
      for (; k + 4 <= words; k += 4) {
        const uint32_t a0 = s[k], a1 = s[k + 1], a2 = s[k + 2], a3 = s[k + 3];
        d[k] = a0, d[k + 1] = a1, d[k + 2] = a2, d[k + 3] = a3;
      }
      for (; k < words; ++k) d[k] = s[k];
    }
  }
  return w;
}

// The full canvas's rows in [from, to) that changed (c.edge_due), out in bands: even windows, as the
// controller wants, each before the scan reaches it (te_gate), the bits cleared as they go.
uint32_t send_edge_rows(CartRun& c, int from, int to, int& k, bool& first) {
  if (!c.full || !c.hd) return 0;
  auto due = [&](int y) { return y < to && (c.edge_due[y >> 5] >> (y & 31) & 1); };
  const int stride = dev->width();
  uint32_t rows = 0;
  for (int y = from & ~1; y < to;) {
    if (!due(y) && !due(y + 1)) {
      y += 2;
      continue;
    }
    const int y0 = y;
    while (y < to && y - y0 < kBandRows && (due(y) || due(y + 1))) y += 2;
    const int y1 = std::min(y, to);
    dev->band_wait(k);
    uint16_t* b = dev->band(k);
    int x0 = stride, x1 = 0;  // the band's columns: those its due rows changed in
    for (int r = y0; r < y1; ++r) {
      edge_row(b + static_cast<size_t>(r - y0) * stride, stride, r, c);
      if (c.edge_due[r >> 5] >> (r & 31) & 1) {
        const bool all = !c.edge_span1[r];
        x0 = std::min<int>(x0, all ? 0 : c.edge_span0[r]);
        x1 = std::max<int>(x1, all ? stride : c.edge_span1[r]);
      }
      c.edge_due[r >> 5] &= ~(1u << (r & 31));
      c.edge_span1[r] = 0;
    }
    const int w = pack_columns(b, stride, y1 - y0, x0, x1);
    te_gate(y0, y1 - 1, first);
    first = false;
    cart_write(c, x0, y0, w, y1 - y0, b, k);
    k ^= 1;
    rows += static_cast<uint32_t>(y1 - y0);
  }
  return rows;
}

// Rows [r0, r1) of an n-wide picture (at screen x, y its row 0) out through band buffer k, as
// rows the glass's width (the picture's edge colours out to the edges), with the cart's
// full-colour layer under or over it.
void cart_band(const uint8_t* pic, int n, int r0, int r1, int x, int y, int& k, bool& first, const CartRun& c, int x0, int x1) {
  dev->band_wait(k);
  uint16_t* b = dev->band(k);
  const int stride = dev->width();
  for (int r = r0; r < r1; ++r) cart_row(b + static_cast<size_t>(r - r0) * kCartScale * stride, stride, x, pic, n, r, c);
  const int y0 = y + r0 * kCartScale, h = (r1 - r0) * kCartScale;
  const int w = pack_columns(b, stride, h, x0, x1);  // only the columns that changed go
  te_gate(y0, y0 + h - 1, first);
  first = false;
  cart_write(c, x0, y0, w, h, b, k);
  k ^= 1;
}

// The sender (core 0): the rows of the picture that changed, in runs of even pairs (the
// controller wants even windows), and the pad when it changed.
void cart_send_frame(CartRun& c) {
  const int64_t t0 = esp_timer_get_time();
  const int64_t wait0 = te.wait_total_us;
  if (const ScanClock sc = scan_clock(); sc.ok) {
    const int64_t into = (t0 - sc.fall) % sc.period;
    c.tx_phase_us += into < 0 ? into + sc.period : into;
  }
  te.frame_open = false;
  te.race_pass = c.tx_race_pass;  // te_gate: bands just ahead of that pass's scan while they can be
  te.race_failed = false;
  te.race_bands = 0;
  te.ahead_t0 = c.tx_t0;
  int k = 0;
  bool first = true;
  uint32_t rows = 0;
  const int glass_w = dev->width();
  auto layer = [&](int r) { return (c.layer_due[r >> 5] >> (r & 31) & 1) != 0; };
  auto changed = [&](int r) { return layer(r) || std::memcmp(c.tx + r * carts::kW, c.shown + r * carts::kW, carts::kW) != 0; };
  // Picture row r's glass columns to send: where the picture changed (its edge columns carry out to
  // the glass's edges), and where the layer did.
  auto span = [&](int r, int& x0, int& x1) {
    const uint8_t* t = c.tx + r * carts::kW;
    const uint8_t* s = c.shown + r * carts::kW;
    int a = 0, z = carts::kW;
    while (a < z && t[a] == s[a]) ++a;
    while (z > a && t[z - 1] == s[z - 1]) --z;
    if (a < z) {
      x0 = std::min(x0, a == 0 ? 0 : kCartX + a * kCartScale);
      x1 = std::max(x1, z == carts::kW ? glass_w : kCartX + z * kCartScale);
    }
    if (layer(r)) {
      x0 = std::min<int>(x0, c.pic_span1[r] ? c.pic_span0[r] : 0);
      x1 = std::max<int>(x1, c.pic_span1[r] ? c.pic_span1[r] : glass_w);
      c.layer_due[r >> 5] &= ~(1u << (r & 31));
    }
  };
  rows += send_edge_rows(c, 0, kCartY, k, first);  // a full canvas: its rows above the window first (the scan's order)
  for (int r = 0; r < carts::kH;) {
    if (!changed(r) && !changed(r + 1)) {
      r += 2;
      continue;
    }
    const int r0 = r;
    while (r < carts::kH && r - r0 < kBandCartRows && (changed(r) || changed(r + 1))) r += 2;
    int x0 = glass_w, x1 = 0;
    for (int q = r0; q < r; ++q) span(q, x0, x1);
    cart_band(c.tx, carts::kW, r0, r, kCartX, kCartY, k, first, c, x0, x1);
    std::memcpy(c.shown + r0 * carts::kW, c.tx + r0 * carts::kW, static_cast<size_t>(r - r0) * carts::kW);
    rows += r - r0;
  }
  rows += send_edge_rows(c, kCartY + kCartPx, kGlassRows, k, first);  // and below it
  dev->band_wait(0);
  dev->band_wait(1);
  if (te.race_pass >= 0 && te.race_bands) {
    if (te.race_failed) ++c.fell_back;
    else ++c.raced;
  }
  te.race_pass = -1;
  if (rows) lat_account(c.tx_age, c.tx_t0, esp_timer_get_time());  // a finger down: when this frame reaches the glass
  const int64_t dt = esp_timer_get_time() - t0;
  c.rows_sent += rows;
  c.tx_frames += 1;
  c.tx_wait_us += te.wait_total_us - wait0;
  c.tx_us += dt;
  if (dt > c.tx_max.load()) c.tx_max = dt;
}

// The sender (core 0) with a stage: every band drawn into a band buffer just before it goes out,
// the whole window each frame (a 3D scene changes most of it).
constexpr int kStageBand = kBandRows;  // 32 rows of 384: the band buffers hold 32 rows of 410
static_assert(carts::Stage::kH % kStageBand == 0, "whole bands");
void stage_helper(void* arg) {
  auto& c = *static_cast<CartRun*>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (c.help_quit.load()) break;
    const int64_t t0 = esp_timer_get_time();
    c.raster_tris += c.stage->band(c.help_y, c.help_y + kStageBand, dev->band(c.help_k), c.depth, c.help_x, -1);
    const int64_t dt = esp_timer_get_time() - t0;
    c.help_us += dt;
    c.help_total += dt;
    c.help_frame_acc += dt;
    xSemaphoreGive(c.help_done);
  }
  c.helper_alive = false;
  vTaskDelete(nullptr);
}

void stage_send_frame(CartRun& c) {
  // {"cart_profile": n}: a stage's drawing is on this core, so its sampler starts here.
  const int prof = cart_profile_core.load() == 1 ? 0 : cart_profile_n.load();
  if (prof && c.prof_left == 0 && dev->profile_start()) c.prof_left = prof;
  const int64_t t0 = esp_timer_get_time();
  te.frame_open = false;
  te.race_pass = c.tx_race_pass;
  te.race_failed = false;
  te.race_bands = 0;
  te.ahead_t0 = c.tx_t0;
  int k = 0, tris = 0;
  int64_t raster = 0, own = 0;
  const int split = c.split.load();
  c.help_frame_acc = 0;
  const ScanClock sc = scan_clock();
  int64_t need = 0;  // the lead this frame needed: each band drawn (from the frame's start) less the scan's time to its top
  bool first = true;  // the frame's first band (te_gate opens the frame there)
  const uint32_t edge_rows = send_edge_rows(c, 0, kCartY, k, first);  // a full canvas: its rows above the window first
  for (int y = 0; y < carts::Stage::kH; y += kStageBand, k ^= 1) {
    dev->band_wait(k);
    const int64_t r0 = esp_timer_get_time();
    if (c.helper) {  // the right half on core 1 meanwhile
      c.help_y = y;
      c.help_k = k;
      c.help_x = split;
      xTaskNotifyGive(c.helper);
      const int64_t o0 = esp_timer_get_time();
      tris += c.stage->band(y, y + kStageBand, dev->band(k), c.depth, 0, split);
      own += esp_timer_get_time() - o0;
      xSemaphoreTake(c.help_done, portMAX_DELAY);
    } else {
      tris += c.stage->band(y, y + kStageBand, dev->band(k), c.depth);
    }
    // The layer's canvas: the window (column 0 at the picture's), or a full canvas (the glass's).
    const int lx = (c.full ? 0 : kCartX) - c.stage_x, ly = (c.full ? kCartY : 0) + y;
    if (c.hd && !c.hd->over()) layer_rows(*c.hd, dev->band(k), c.stage->width(), lx, ly, kStageBand);
    if (c.overlay)  // a 3D cart's picture over its scene
      carts::overlay_rows(dev->band(k), c.stage->width(), kCartX - c.stage_x, y, kStageBand, c.tx, c.pal, c.dim);
    if (c.hd && c.hd->over()) layer_rows(*c.hd, dev->band(k), c.stage->width(), lx, ly, kStageBand);
    const int64_t r1 = esp_timer_get_time();
    raster += r1 - r0;
    // (te_gate races a band only with its transfer and a guard still before the scan: ~1 ms)
    if (sc.ok) need = std::max(need, r1 - c.tx_t0 - sc.at(kCartY + y) + kStageBandSendUs);
    const bool was_failed = te.race_failed;
    te_gate(kCartY + y, kCartY + y + kStageBand - 1, first);
    first = false;
    if (te.race_pass >= 0 && te.race_failed && !was_failed && y > 0 && sc.ok) {  // a seam: where, and by how much (logged with the stats)
      c.seam_band = y / kStageBand;
      c.seam_slack = sc.start(c.tx_race_pass) + sc.at(kCartY + y) - r1;
      c.seam_draw = r1 - r0;
    }
    c.band_draw[y / kStageBand] += r1 - r0;
    c.band_ready[y / kStageBand] += r1 - t0;
    c.band_out[y / kStageBand] += esp_timer_get_time() - t0;
    dev->band_send(k, c.stage_x, kCartY + y, c.stage->width(), kStageBand);
  }
  const uint32_t edge_below = send_edge_rows(c, kCartY + kCartPx, kGlassRows, k, first);  // and below it
  dev->band_wait(0);
  dev->band_wait(1);
  if (te.race_pass >= 0 && te.race_bands) {
    if (te.race_failed) ++c.fell_back;
    else ++c.raced;
  }
  te.race_pass = -1;
  lat_account(c.tx_age, c.tx_t0, esp_timer_get_time());
  const int64_t dt = esp_timer_get_time() - t0;
  c.rows_sent += carts::Stage::kH + edge_rows + edge_below;
  c.tx_frames += 1;
  c.tx_us += dt;
  if (dt > c.tx_max.load()) c.tx_max = dt;
  c.raster_us += raster;
  if (raster > c.raster_max.load()) c.raster_max = raster;
  c.raster_tris += tris;
  c.own_frame = own;
  c.help_frame = c.help_frame_acc.load();
  c.raster_frame = raster;
  if (sc.ok) c.need_lead = need;
  c.split_avg_acc += split;
  if (cart_profile_core.load() != 1 && c.prof_left && --c.prof_left == 0) {
    cart_profile_n = 0;
    char what[64];
    snprintf(what, sizeof what, "stage %s, %d frames (core 0)", c.stage_name.c_str(), prof ? prof : 0);
    dev->profile_stop(what);
  }
}

// {"panel_check": n}: the glass as the sender has written it (its copy: the panel can't be read
// back) against the whole frame composed afresh, after each of n frames. A column the narrowed
// windows missed would show here. The first frame sends everything again, to fill the copy.
void panel_check(CartRun& c) {
  const int W = dev->width();
  if (!c.shadow) {
    if (c.stage || cart_panel_check.load() <= 0) return;
    c.shadow = static_cast<uint16_t*>(heap_caps_malloc(static_cast<size_t>(W) * kGlassRows * 2, MALLOC_CAP_SPIRAM));
    c.check_left = cart_panel_check.exchange(0);
    if (!c.shadow) return;
    c.check_frames = c.check_bad = 0;
    c.check_x = c.check_y = -1;
    std::memset(c.shown, 0xFF, carts::kW * carts::kH);  // all of it next frame (the sender's own state: safe here)
    std::memset(c.edge_due, 0xFF, sizeof c.edge_due);
    std::memset(c.edge_span1, 0, sizeof c.edge_span1);
    return;
  }
  std::vector<uint16_t> rows(static_cast<size_t>(W) * kCartScale);
  auto compare = [&](const uint16_t* want, int y) {
    const uint16_t* got = c.shadow + static_cast<size_t>(y) * W;
    for (int x = 0; x < W; ++x)
      if (got[x] != want[x] && c.check_bad++ == 0) c.check_x = x, c.check_y = y;
  };
  for (int r = 0; r < carts::kH; ++r) {
    cart_row(rows.data(), W, kCartX, c.tx, carts::kW, r, c);
    for (int i = 0; i < kCartScale; ++i) compare(rows.data() + static_cast<size_t>(i) * W, kCartY + r * kCartScale + i);
  }
  if (c.full && c.hd)
    for (int y = 0; y < kGlassRows; ++y)
      if (y < kCartY || y >= kCartY + kCartPx) {
        edge_row(rows.data(), W, y, c);
        compare(rows.data(), y);
      }
  ++c.check_frames;
  if (--c.check_left > 0) return;
  if (c.check_bad)
    ESP_LOGW(kTag, "cart %s: panel check: %d pixels not as composed in %d frames; the first at %d, %d", c.cart.name().c_str(),
             c.check_bad, c.check_frames, c.check_x, c.check_y);
  else
    ESP_LOGI(kTag, "cart %s: panel check: the glass as composed, %d frames", c.cart.name().c_str(), c.check_frames);
  heap_caps_free(c.shadow);
  c.shadow = nullptr;
}

void cart_sender(void* arg) {
  auto& c = *static_cast<CartRun*>(arg);
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if (c.quit.load()) break;
    dev->frame_out_begin();  // (a host napping between a slow cart's frames: full speed while this one goes out)
    if (c.stage) stage_send_frame(c);
    else cart_send_frame(c);
    panel_check(c);
    dev->frame_out_end();
    // Done with the layer's front rows until the next publish(): the cart draws on them in place
    // from here (a screenshot taken meanwhile may catch a row mid-drawing; nothing else reads them).
    if (c.hd) c.hd->release_front();
    c.busy = false;
  }
  c.alive = false;
  vTaskDelete(nullptr);
}

void cart_close();

// The ticking task: a stage starts drawing (the test's, or a cart's once it makes anything 3D): its work
// cells in internal RAM, the helper on core 1, the sender at the stage's priority, and both frames
// prepared (the next tick publishes one). False: no memory.
bool stage_enable(CartRun& c) {
  const size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (!c.stage) {
    c.stage = c.cart.stage();
    c.overlay = true;
    c.stage_name = c.cart.name();
  }
  c.depth = static_cast<uint16_t*>(heap_caps_malloc(c.stage->work_cells() * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  if (!c.depth) {
    ESP_LOGE(kTag, "stage %s: no internal RAM for its work cells (%u bytes; largest free block %u)", c.stage_name.c_str(),
             static_cast<unsigned>(c.stage->work_cells() * 2), static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    c.stage = nullptr;
    return false;
  }
  c.help_done = xSemaphoreCreateBinary();
  c.helper_alive = true;
  if (!c.help_done || xTaskCreatePinnedToCore(stage_helper, "hg-stage-help", 3072, &c, g_cfg.frame_prio + 2, &c.helper, 1) != pdPASS) {
    ESP_LOGW(kTag, "stage %s: no helper task: one core draws", c.stage_name.c_str());
    c.helper_alive = false;
    c.helper = nullptr;
  }
  // The sender draws now: priority 10, under wifi and lwip, over the socket and touch tasks (at 3
  // those preempted it mid-band; at 19 it held them off for whole frames).
  if (c.sender) vTaskPrioritySet(c.sender, 10);
  c.stage_t0 = esp_timer_get_time();
  // A stage as wide as the glass starts at its left edge; a window-wide one (the half-size test) at the window
  c.stage_x = c.stage->width() >= static_cast<int>(dev->width()) ? 0 : kCartX;
  c.split = c.stage->width() / 2 / 4 * 4;
  for (auto& v : c.lua_recent) v = 6000;  // the lead before frames measure it
  c.stage->prepare();
  c.stage->publish();
  c.stage->prepare();
  ESP_LOGI(kTag, "stage %s: %d triangles in its shapes; internal RAM %d bytes; largest free block now %u, PSRAM free %u KB",
           c.stage_name.c_str(), c.stage->triangles(), static_cast<int>(heap0) - static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
           static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
  return true;
}

// The host's task, under its lock: the host's screen steps aside (Device::will_open) and the cart starts.
bool cart_open(const std::string& name) {
  // The 3D stage test: "@stage" (384x384, rings of 28 segments), "@stage<segments>",
  // "@stagehalf<segments>" (192x192 doubled), "@stagejet<segments>" (Jet's own pipeline and
  // rasteriser at 192x192, for comparison).
  const bool stage = name.rfind("@stage", 0) == 0;
  const std::string src = stage ? std::string() : g_lib->source(name);
  if (src.empty() && !stage) {
    ESP_LOGW(kTag, "cart %s: no such cart", name.c_str());
    return false;
  }
  if (g_cart) cart_close();
  dev->will_open();
  auto* c = new CartRun(*dev);
  c->tx = static_cast<uint8_t*>(heap_caps_malloc(carts::kW * carts::kH, MALLOC_CAP_SPIRAM));
  c->shown = static_cast<uint8_t*>(heap_caps_malloc(carts::kW * carts::kH, MALLOC_CAP_SPIRAM));
  if (!c->tx || !c->shown) {
    heap_caps_free(c->tx);
    heap_caps_free(c->shown);
    delete c;
    ESP_LOGE(kTag, "cart %s: no memory", name.c_str());
    dev->closed();
    return false;
  }
  std::memset(c->shown, 0xFF, carts::kW * carts::kH);
  set_colours(*c);
  esp_timer_create_args_t ta = {};
  ta.callback = [](void*) { dev->wake_screen(); };  // a cart's wake(): light the screen; the cart resumes
  ta.name = "hg-cart-wake";
  esp_timer_create(&ta, &c->host.wake_timer);
  // A frame that runs too long: a 1 MHz timer whose interrupt (on this core, Lua's) hooks the VM,
  // as a signal would. No instruction counting while it runs well.
  c->host.cart = &c->cart;
  if (!stage) c->host.load_images(name);
  {
    gptimer_config_t tc = {};
    tc.clk_src = GPTIMER_CLK_SRC_DEFAULT;
    tc.direction = GPTIMER_COUNT_UP;
    tc.resolution_hz = 1000000;
    gptimer_event_callbacks_t cb = {};
    cb.on_alarm = [](gptimer_handle_t, const gptimer_alarm_event_data_t*, void* arg) -> bool {
      static_cast<carts::Cart*>(arg)->interrupt();
      return false;
    };
    if (gptimer_new_timer(&tc, &c->host.deadline_timer) != ESP_OK ||
        gptimer_register_event_callbacks(c->host.deadline_timer, &cb, &c->cart) != ESP_OK) {
      ESP_LOGW(kTag, "cart %s: no deadline timer: a runaway frame can't be stopped", name.c_str());
      if (c->host.deadline_timer) gptimer_del_timer(c->host.deadline_timer);
      c->host.deadline_timer = nullptr;
    }
  }
  g_slow = false;
  g_cart = c;
  vTaskPrioritySet(nullptr, g_cfg.frame_prio);  // frames on the pass, ahead of the app and websocket tasks
  dev->clear_glass();
  const int64_t t0 = esp_timer_get_time();
  const size_t heap0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  if (stage) {
    const bool half = name.rfind("@stagehalf", 0) == 0, jet = name.rfind("@stagejet", 0) == 0;
    const char* n = name.c_str() + (half ? 10 : jet ? 9 : 6);
    const int segs = std::clamp(*n ? std::atoi(n) : 28, 8, 96);
    c->own_stage = half || jet ? std::make_unique<carts::Stage>(2) : std::make_unique<carts::Stage>(1, carts::kStageW, carts::kStageX + carts::Stage::kW / 2.0f);
    c->stage = c->own_stage.get();
    c->stage->build_rings(segs);
    c->stage->use_jet_raster(jet);
    c->stage_name = name;
    c->stage->step(0, 0, 0);
    ESP_LOGI(kTag, "stage %s: %d triangles, built in %.1f ms", name.c_str(), c->stage->triangles(), (esp_timer_get_time() - t0) / 1000.0);
  } else {
    const bool ok = c->cart.start(name, src);
    c->full = c->cart.full_canvas();
    ESP_LOGI(kTag, "cart %s: %s in %.1f ms, %d fps, Lua heap %u KB, internal RAM %d bytes, task stack left %u%s%s", name.c_str(),
             ok ? "started" : "stopped", (esp_timer_get_time() - t0) / 1000.0, c->cart.fps(),
             static_cast<unsigned>(c->cart.memory() / 1024), static_cast<int>(heap0) - static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)), ok ? "" : ": ", ok ? "" : c->cart.error().c_str());
  }
  c->alive = true;
  // (4 KB: composing a row may take an aligned row of 832 bytes on the stack for the vector loops)
  if (xTaskCreatePinnedToCore(cart_sender, "hg-cart-tx", 4096, c, 19, &c->sender, 0) != pdPASS) {
    ESP_LOGE(kTag, "cart %s: no sender task", name.c_str());
    c->alive = false;
    cart_close();
    return true;  // (it was there: closed again, logged)
  }
  if ((c->stage || c->cart.stage()) && !stage_enable(*c)) {  // the test, or a cart that made something 3D in _init
    cart_close();
    return true;
  }
  c->window_t0 = esp_timer_get_time();
  dev->note_activity();
  g_open_name = name;
  dev->command("cart", name);  // the host sends a cart's own channels only while it's open
  return true;
}

// The host's task: the cart goes and the host's screen comes back (Device::closed).
void cart_close() {
  CartRun* c = g_cart;
  if (!c) return;
  c->quit = true;
  if (c->sender) xTaskNotifyGive(c->sender);
  for (int i = 0; i < 100 && c->alive.load(); ++i) vTaskDelay(1);
  if (c->alive.load()) dev->timed_out("the cart sender's stop");
  if (c->helper) {  // after the sender: a frame it was sending needs the helper to the end
    c->help_quit = true;
    xTaskNotifyGive(c->helper);
    for (int i = 0; i < 100 && c->helper_alive.load(); ++i) vTaskDelay(1);
    if (c->helper_alive.load()) dev->timed_out("the stage helper's stop");
  }
  if (c->help_done) vSemaphoreDelete(c->help_done);
  dev->band_wait(0);
  dev->band_wait(1);
  if (c->host.wake_timer) {
    esp_timer_stop(c->host.wake_timer);
    esp_timer_delete(c->host.wake_timer);
  }
  if (c->host.deadline_timer) {
    c->host.deadline(0);  // stopped and disabled, if a frame was running
    gptimer_del_timer(c->host.deadline_timer);
  }
  if (c->imu_on && dev->has_imu()) dev->imu_enable(false);
  if (dev->has_motor()) dev->motor_rest();
  ESP_LOGI(kTag, "cart %s: closed%s%s", c->stage ? c->stage_name.c_str() : c->cart.name().c_str(), c->cart.error().empty() ? "" : " after: ",
           c->cart.error().c_str());
  g_cart = nullptr;
  g_open_name.clear();
  dev->command("cart", "");
  heap_caps_free(c->tx);
  heap_caps_free(c->shadow);  // (a panel check still running)
  heap_caps_free(c->shown);
  heap_caps_free(c->depth);
  delete c;
  vTaskPrioritySet(nullptr, g_cfg.idle_prio);
  dev->closed();
  dev->note_activity();
}

// Every 5 s while a cart runs: its frame rate, Lua's share, what went out.
void cart_stats(CartRun& c) {
  const int64_t now = esp_timer_get_time();
  if (now - c.window_t0 < kStatsUs) return;
  const float secs = (now - c.window_t0) / 1e6f;
  const uint32_t txf = c.tx_frames.exchange(0);
  const int64_t txus = c.tx_us.exchange(0), txmax = c.tx_max.exchange(0);
  const int64_t txwait = c.tx_wait_us.exchange(0), txphase = c.tx_phase_us.exchange(0);
  const uint32_t rows = c.rows_sent.exchange(0);
  const uint32_t px = c.px_sent.exchange(0), px_rows = c.px_rows.exchange(0);
  const uint32_t raced = c.raced.exchange(0), fell = c.fell_back.exchange(0);
  if (c.stage) {
    const int64_t rus = c.raster_us.exchange(0), rmax = c.raster_max.exchange(0);
    const uint32_t tris = c.raster_tris.exchange(0);
    const int64_t hus = c.help_us.exchange(0);
    if (c.frames)
      ESP_LOGI(kTag, "stage %s: %u frames in %.1f s (%.1f/s, %u late, %u dropped); Lua %.2f ms avg, %.2f max (%.2f its own, %u KB freed in it); prepare (core 1) %.2f ms avg, %.2f max; "
                    "drawing (core 0) %.2f ms avg, %.2f max, %u triangles queued, %.0f drawn in bands; sent %.2f ms avg, %.2f max; "
                    "internal free %u (low %u); sender stack left %u; start late %.2f ms avg, %.2f max; bands late %u of %u; helper (core 1) %.2f ms a frame; split at %d",
               c.stage_name.c_str(), static_cast<unsigned>(c.frames), secs, c.frames / secs, static_cast<unsigned>(c.late),
               static_cast<unsigned>(c.dropped), c.lua_us / 1000.0 / c.frames, c.lua_max / 1000.0, c.lua_own_max / 1000.0, static_cast<unsigned>(c.lua_max_freed / 1024),
               c.prep_us / 1000.0 / c.frames,
               c.prep_max / 1000.0, txf ? rus / 1000.0 / txf : 0.0,
               rmax / 1000.0, static_cast<unsigned>(c.stage->queued()), txf ? static_cast<double>(tris) / txf : 0.0,
               txf ? txus / 1000.0 / txf : 0.0, txmax / 1000.0, static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
               static_cast<unsigned>(uxTaskGetStackHighWaterMark(c.sender)), c.start_late_us / 1000.0 / c.frames,
               c.start_late_max / 1000.0, static_cast<unsigned>(te.late - c.te_late0), static_cast<unsigned>(te.bands - c.te_bands0),
               txf ? hus / 1000.0 / txf : 0.0, txf ? static_cast<int>(c.split_avg_acc.exchange(0) / static_cast<int>(txf)) : 0);
    c.prep_us = c.prep_max = 0;
    c.te_late0 = te.late;
    c.te_bands0 = te.bands;
    int64_t lead = 0;
    for (const int64_t v : c.lua_recent) lead = std::max(lead, v);
    ESP_LOGI(kTag, "stage %s: raced %u frames, fell back %u (whole, a pass later), seams %u; lead %.1f ms%s; drawn at %s size%s", c.stage_name.c_str(),
             static_cast<unsigned>(raced), static_cast<unsigned>(fell), static_cast<unsigned>(te.ahead_late - c.seams0),
             (lead + kStageRaceMarginUs) / 1000.0, lead + kStageRaceMarginUs < kCartRaceMaxLeadUs ? "" : " (too long: behind the scan)",
             c.stage->scale() == 2 ? "half" : "full", c.stage->auto_res() ? " (auto)" : "");
    c.seams0 = te.ahead_late;
    if (c.seam_band >= 0) {
      ESP_LOGW(kTag, "stage %s: the last seam at band %d, drawn %.2f ms before the scan reached it (drawing %.2f ms)", c.stage_name.c_str(),
               c.seam_band, c.seam_slack / 1000.0, c.seam_draw / 1000.0);
      c.seam_band = -1;
    }
    if (lat.n) {
      const double ln = lat.n;
      ESP_LOGI(kTag, "stage %s: touch to photon %.1f ms (sample age %.1f + latch to mid-screen %.1f), %u frames", c.stage_name.c_str(),
               (lat.age + lat.scan) / 1000.0 / ln, lat.age / 1000.0 / ln, lat.scan / 1000.0 / ln, static_cast<unsigned>(lat.n));
      lat = {};
    }
    if (txf) {
      char b[400];
      int o = 0;
      for (int i = 0; i < 12; ++i)
        o += snprintf(b + o, sizeof b - o, "%d: %.1f/%.1f/%.1f  ", i, c.band_draw[i] / 1000.0 / txf, c.band_ready[i] / 1000.0 / txf,
                      c.band_out[i] / 1000.0 / txf);
      ESP_LOGI(kTag, "stage bands (draw / drawn by / gate open by, ms from the frame's start): %s", b);
    }
    for (int i = 0; i < 12; ++i) c.band_draw[i] = c.band_ready[i] = c.band_out[i] = 0;
    c.start_late_us = c.start_late_max = 0;
    (void)rows;
    c.window_t0 = now;
    c.frames = c.late = c.dropped = 0;
    c.lua_us = c.lua_max = 0;
  c.lua_max_freed = 0;
    c.lua_own_max = 0;
    c.lua_max_freed = 0;
    c.lua_own_max = 0;
    return;
  }
  if (c.table)
    ESP_LOGI(kTag, "cart %s: tilt %+.2f %+.2f (chip %+.2f %+.2f %+.2f g)", c.cart.name().c_str(), c.tilt_x, c.tilt_y, c.acc[0], c.acc[1], c.acc[2]);
  if (lat.n) {
    const double n = lat.n;
    ESP_LOGI(kTag, "cart %s: touch to photon %.1f ms (sample age %.1f + latch to mid-screen %.1f), %u frames; raced %u, fell back %u",
             c.cart.name().c_str(), (lat.age + lat.scan) / 1000.0 / n, lat.age / 1000.0 / n, lat.scan / 1000.0 / n,
             static_cast<unsigned>(lat.n), static_cast<unsigned>(raced), static_cast<unsigned>(fell));
    lat = {};
  } else if (raced || fell) {
    ESP_LOGI(kTag, "cart %s: raced %u frames, fell back %u", c.cart.name().c_str(), static_cast<unsigned>(raced), static_cast<unsigned>(fell));
  }
  c.cart.set_rates(c.frames / secs, te.period_us.load() > 0 ? static_cast<float>(te.period_us.load()) * (60 / c.cart.fps())
                                                             : 1e6f / c.cart.fps());
  if (c.frames)
    ESP_LOGI(kTag, "cart %s: %u frames in %.1f s (%.1f/s, %u late, %u dropped, lock wait max %.1f ms); Lua %.2f ms avg, %.2f max; "
                  "sent %u frames, %.1f rows/frame, %.2f ms avg (%.2f waiting for the scan, handed over %.1f ms into a pass), %.2f max, %.1f kpx/frame (of %.1f in its rows); heap %u KB; internal free %u (low %u)",
             c.cart.name().c_str(), static_cast<unsigned>(c.frames), secs, c.frames / secs, static_cast<unsigned>(c.late),
             static_cast<unsigned>(c.dropped), dev->lock_wait_ms(false),
             c.lua_us / 1000.0 / c.frames, c.lua_max / 1000.0, static_cast<unsigned>(txf),
             txf ? static_cast<double>(rows) / txf : 0.0, txf ? txus / 1000.0 / txf : 0.0, txf ? txwait / 1000.0 / txf : 0.0,
             txf ? txphase / 1000.0 / txf : 0.0, txmax / 1000.0,
             txf ? px / 1000.0 / txf : 0.0, txf ? px_rows / 1000.0 / txf : 0.0, static_cast<unsigned>(c.cart.memory() / 1024), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)));
  if (c.frames && c.hd)
    ESP_LOGI(kTag, "cart %s: layer publish %.2f ms avg, %.2f max; others' lock wait max %.1f ms", c.cart.name().c_str(),
             c.hd_us / 1000.0 / c.frames, c.hd_max / 1000.0, dev->lock_wait_ms(true));
  c.window_t0 = now;
  c.frames = c.late = c.dropped = 0;
  c.lua_us = c.lua_max = 0;
  c.lua_max_freed = 0;
    c.lua_own_max = 0;
  c.hd_us = c.hd_max = 0;
}

// Each loop of the ticking task while a cart is open: a frame when its pass comes. Returns how long the loop
// may sleep (ms).
// The virtual table for tilt(): level in the pose the board is in now. The screen's right and down
// in chip axes come from the mounting (g_tilt: tilt = axis * sign, so the axis is the
// reaction's direction and the screen direction is its opposite); each is laid flat on the plane
// at right angles to gravity now (Gram-Schmidt).
void tilt_table(CartRun& c) {
  const float n = std::sqrt(c.acc[0] * c.acc[0] + c.acc[1] * c.acc[1] + c.acc[2] * c.acc[2]);
  if (n < 0.5f) return;  // in free fall or not read yet: keep the old table
  const float u[3] = {c.acc[0] / n, c.acc[1] / n, c.acc[2] / n};
  auto flat = [&](float* v, int axis, int sign, const float* other) {
    for (int i = 0; i < 3; ++i) v[i] = 0;
    v[axis % 3] = -static_cast<float>(sign);
    float k = v[0] * u[0] + v[1] * u[1] + v[2] * u[2];
    for (int i = 0; i < 3; ++i) v[i] -= k * u[i];
    if (other) {
      k = v[0] * other[0] + v[1] * other[1] + v[2] * other[2];
      for (int i = 0; i < 3; ++i) v[i] -= k * other[i];
    }
    const float m = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (m > 1e-3f)
      for (int i = 0; i < 3; ++i) v[i] /= m;
  };
  flat(c.r, g_tilt[0], g_tilt[1], nullptr);
  flat(c.d, g_tilt[2], g_tilt[3], c.r);
  c.table = true;
  c.relevel = false;
  c.host.level_due = false;
}

uint32_t cart_tick() {
  CartRun& c = *g_cart;
  if (!dev->screen_on()) {  // dark: paused (no frames); the wake starts from a fresh pass
    c.next_pass = -1;
    if (c.imu_on && dev->has_imu()) c.imu_on = !dev->imu_enable(false);
    return 200;
  }
  const ScanClock sc = scan_clock();
  const int step = c.stage && !c.overlay ? 1 : 60 / c.cart.fps();  // passes a frame (fps() divides 60)
  g_slow = !c.stage && step >= 6;  // a frame every 100 ms or more: it may nap between them (lit_idle)
  int64_t now = esp_timer_get_time(), due;
  // A lazy cart (fps(active, idle)) idling at a few frames a second: a touch coming down or BOOT
  // pressed starts a frame at the next pass it can make, not at its next idle frame. Once: the
  // frame that sees it is what notes the finger (c.finger), and aiming again on every loop until
  // then would keep that frame two passes away for as long as the finger stayed down.
  if (c.cart.lazy() && step > 1 && !c.poked) {
    const bool down = dev->touch().touching;
    if ((down && !c.finger) || (g_button_b.load() && !c.boot_was)) {
      c.poked = true;
      c.next_pass = -1;  // (as a first frame: not counted late)
      c.last_due = 0;
    }
  }
  int64_t lead = 0;
  for (const int64_t v : c.lua_recent) lead = std::max(lead, v);
  // A stage's lua_recent holds each frame's needed lead (its bands are drawn as they go out); its
  // frames start up to ~3 ms late (core 1 busy with the helper's bands at the due time), and a band
  // that misses mid-frame is a seam on the glass: a wider margin.
  const int64_t stage_need = lead;  // a stage: the most its recent frames needed
  lead += c.stage ? kStageRaceMarginUs : kCartRacePrepUs;
  const bool race = sc.ok && te.ahead && lead < kCartRaceMaxLeadUs;
  // Behind the scan, a frame starts at the pass before the one it shows on; racing, `lead` before it.
  auto start_for = [&](int64_t show) { return race ? sc.start(show) - lead : sc.start(show - 1); };
  if (sc.ok) {
    if (c.next_pass < 0 || start_for(c.next_pass) < now - 3000) {  // the first frame, or behind: the next pass it can make
      if (c.next_pass >= 0) ++c.late;
      c.next_pass = static_cast<int64_t>(sc.base) + 2;
      while (start_for(c.next_pass) < now - 1000) ++c.next_pass;
    }
    due = start_for(c.next_pass);
  } else {
    due = c.last_due ? c.last_due + step * 16742 : now;
    if (due < now - 3000) due = now;
  }
  // Sleep up to it, within a millisecond (racing has its margin for that; behind the scan it costs
  // nothing). Spinning the last ~1.5 ms each frame was 7.5% of the core in the profile.
  if (due - now > 300) return static_cast<uint32_t>((due - now + 999) / 1000);
  c.last_due = due;
  c.poked = false;
  const int64_t show = c.next_pass;
  if (sc.ok) c.next_pass += step;
  // The input as it stands now (the touch task's newest sample, BOOT).
  const Touch t = dev->touch();
  const int64_t t_us = t.t_us;
  const int64_t latch = esp_timer_get_time();
  carts::Input in;
  if (t.touching) {
    if (!c.finger) {
      c.down_x = t.down_x;
      c.down_y = t.down_y;
      c.down_us = latch;
      c.moved = false;
      // From above the picture (the glass's top 60 rows; a full canvas's top 24, the rest is its):
      // maybe the swipe that leaves.
      c.edge = c.down_y < (c.full ? kFullCanvasExitStrip : kCartY);
    }
    if (c.edge && t.y - c.down_y > 90) {
      cart_close();
      g_gesture_closed = true;  // the rest of that swipe goes nowhere on the host's screen
      return 1;
    }
    // A finger from the top edge belongs to the leaving gesture, not the cart; a full canvas gets
    // the whole glass (its picture coordinates run past 0-127 there).
    if (!c.edge && (c.full || (t.x >= kCartX && t.x < kCartX + kCartPx && t.y >= kCartY && t.y < kCartY + kCartPx))) {
      in.touching = true;
      in.tx = (t.x - kCartX) / static_cast<float>(kCartScale);
      in.ty = (t.y - kCartY) / static_cast<float>(kCartScale);
    }
    // A drag (anywhere) is the d-pad: its direction from where the finger came down, diagonals too.
    const int dx = t.x - c.down_x, dy = t.y - c.down_y;
    if (dx * dx + dy * dy > kDragPx * kDragPx) c.moved = true;
    if (c.moved && !c.edge) {
      const int ax = std::abs(dx), ay = std::abs(dy);
      if (ax * 5 > ay * 2) in.held |= dx < 0 ? 1 << carts::kLeft : 1 << carts::kRight;
      if (ay * 5 > ax * 2) in.held |= dy < 0 ? 1 << carts::kUp : 1 << carts::kDown;
    }
  } else if (c.finger && !c.moved && !c.edge && latch - c.down_us < kTapMaxUs) {
    c.tap = true;  // a tap: A for one frame, as it lifts
  }
  c.finger = t.touching;
  if (c.tap && !t.touching) {
    in.held |= 1 << carts::kA;
    c.tap = false;
  }
  c.boot_was = g_button_b.load();
  if (c.boot_was) in.held |= 1 << carts::kB;
  // Tilt: the accelerometer, on from the first frame lit, a little smoothed (62.5 Hz, read once a frame).
  if (dev->has_imu()) {
    if (!c.imu_on && (c.imu_on = dev->imu_enable(true))) c.readings = 0;
    float a[3];
    if (c.imu_on && dev->imu_read(&a[0], &a[1], &a[2])) {
      for (int i = 0; i < 3; ++i) c.acc[i] += (a[i] - c.acc[i]) * (c.readings ? 0.5f : 1.0f);  // the first one as it is
      // The table from the settled pose (12 frames, ~0.2 s, after it comes on), or on level().
      ++c.readings;
      if ((c.relevel && c.readings >= 12) || c.host.level_due) tilt_table(c);
    }
    if (c.table) {
      const float n = std::sqrt(c.acc[0] * c.acc[0] + c.acc[1] * c.acc[1] + c.acc[2] * c.acc[2]);
      // Gravity's pull along the table's right and down: the reaction the chip reads, negated.
      if (n > 0.3f) {
        in.ax = std::clamp(-(c.acc[0] * c.r[0] + c.acc[1] * c.r[1] + c.acc[2] * c.r[2]) / n, -1.0f, 1.0f);
        in.ay = std::clamp(-(c.acc[0] * c.d[0] + c.acc[1] * c.d[1] + c.acc[2] * c.d[2]) / n, -1.0f, 1.0f);
      }
      c.tilt_x = in.ax;
      c.tilt_y = in.ay;
    }
  }
  const int prof = c.stage && cart_profile_core.load() != 1 ? 0 : cart_profile_n.load();  // a stage's drawing: sampled on the sender's core
  if (prof && c.prof_left == 0 && dev->profile_start()) c.prof_left = prof;
  // {"cart_perf": n}: n frames for each of the core's events, counted only inside the frames.
  struct PerfEv { uint16_t sel, mask; const char* name; };
  static const PerfEv kPerf[] = {
      {XTPERF_CNT_INSN, 0xFFFF, "instructions"},
      {XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_ALL, "I stall, all"},
      {XTPERF_CNT_I_STALL, XTPERF_MASK_I_STALL_CACHE_MISS, "I stall, cache miss"},
      {XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_ALL, "D stall, all"},
      {XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_CACHE_MISS, "D stall, cache miss"},
      {XTPERF_CNT_D_STALL, XTPERF_MASK_D_STALL_STORE_BUF_FULL, "D stall, store buffer full"},
      {XTPERF_CNT_BUBBLES, XTPERF_MASK_BUBBLES_ALL, "bubbles, all"},
  };
  const int perf_n = cart_perf_n.load();
  if (perf_n && c.perf_frame == 0) {
    xtensa_perfmon_init(0, XTPERF_CNT_CYCLES, 0xFFFF, 0, -1);
    xtensa_perfmon_init(1, kPerf[0].sel, kPerf[0].mask, 0, -1);
    xtensa_perfmon_reset(0);
    xtensa_perfmon_reset(1);
  }
  if (perf_n) xtensa_perfmon_start();
  if (c.stage) {
    // The sender draws the front frame as it sends it: a frame that would start while it's still
    // busy waits for the next pass. Otherwise the frame prepared last time goes to it (and the
    // next is prepared below, on this core, while it draws).
    if (c.busy.load()) {  // the same pass again next loop: behind the scan, its first band can start ~3 ms late
      c.next_pass = show;
      return 1;
    }
    c.stage->publish();
    ++c.frames;
    const int64_t sl = esp_timer_get_time() - due;
    c.start_late_us += sl;
    c.start_late_max = std::max(c.start_late_max, sl);
  } else if (c.cart.running()) {
    const int rate = c.cart.fps();
    c.cart.frame(in);
    ++c.frames;
    // fps() changed it (a lazy cart's input, or the cart's call): the next frame is the new
    // rate's gap after this one, not the old's (an idle gap after the first touch, half a second).
    if (sc.ok && c.cart.fps() != rate) c.next_pass = show + 60 / c.cart.fps();
    c.lua_us += c.cart.last().lua_us;
    if (c.cart.last().lua_us > c.lua_max) c.lua_max_freed = c.cart.last().freed;
        c.lua_max = std::max(c.lua_max, c.cart.last().lua_us);
    c.lua_recent[c.lua_i++ % 16] = c.cart.last().lua_us;
    if (!c.cart.error().empty()) ESP_LOGW(kTag, "cart %s stopped: %s", c.cart.name().c_str(), c.cart.error().c_str());
    if (!c.stage && c.cart.stage() && !stage_enable(c)) {  // it made something 3D: from this frame, its picture over the scene
      cart_close();
      return 1;
    }
  }
  if (perf_n) {
    xtensa_perfmon_stop();
    const int ev = c.perf_frame / perf_n;
    if (++c.perf_frame % perf_n == 0) {
      const uint32_t cyc = xtensa_perfmon_value(0), v = xtensa_perfmon_value(1);
      ESP_LOGI(kTag, "cart perfmon: %-26s %8.1f k a frame (%4.1f%% of %.1f k cycles)", kPerf[ev].name, v / 1000.0 / perf_n,
               100.0 * v / std::max<uint32_t>(cyc, 1), cyc / 1000.0 / perf_n);
      if (ev + 1 < static_cast<int>(sizeof kPerf / sizeof kPerf[0])) {
        xtensa_perfmon_init(1, kPerf[ev + 1].sel, kPerf[ev + 1].mask, 0, -1);
        xtensa_perfmon_reset(0);
        xtensa_perfmon_reset(1);
      } else {
        cart_perf_n = 0;
        c.perf_frame = 0;
      }
    }
  }
  if ((!c.stage || cart_profile_core.load() == 1) && c.prof_left && --c.prof_left == 0) {  // only the frames' own time is sampled (the timer runs between them too)
    cart_profile_n = 0;
    char what[64];
    snprintf(what, sizeof what, "cart %s, %d frames", c.cart.name().c_str(), prof);
    dev->profile_stop(what);
  }
  // To the sender: the last frame is normally long gone (it went out behind the last pass's scan).
  for (int i = 0; i < 30 && c.busy.load(); ++i) vTaskDelay(1);
  if (c.busy.load()) {  // the sender still on the last frame after 30 ms: this one isn't shown
    ++c.dropped;
    dev->timed_out("the cart sender (a frame dropped)");
  } else {
    if (!c.stage || c.overlay) std::memcpy(c.tx, c.cart.picture(), carts::kW * carts::kH);  // a 3D cart's: last tick's, as its scene
    if (c.cart.colours_version() != c.pal_ver) {  // palette(): new colours (the sender is idle: safe here)
      set_colours(c);
      std::memset(c.shown, 0xFF, carts::kW * carts::kH);  // every row again, in them
      if (c.full) {  // and a full canvas's others (the layer shows over colour 0 there), all of each
        std::memset(c.edge_due, 0xFF, sizeof c.edge_due);
        std::memset(c.edge_span1, 0, sizeof c.edge_span1);
      }
    }
    c.hd = c.cart.hd();
    const int64_t h0 = esp_timer_get_time();
    const bool hd_moved = c.hd && c.hd->publish();
    if (c.hd) {
      const int64_t h = esp_timer_get_time() - h0;
      c.hd_us += h;
      c.hd_max = std::max(c.hd_max, h);
    }
    if (hd_moved) {  // the layer's changed rows go again (the picture rows over them; a full canvas's others), as wide as they changed
      const auto& moved = c.hd->changed();
      const int gw = dev->width(), cw = c.hd->width();
      for (int y = 0; y < c.hd->height(); ++y) {
        if (!(moved[y >> 5] >> (y & 31) & 1)) continue;
        int x0 = 0, x1 = 0;
        c.hd->changed_span(y, &x0, &x1);
        if (!c.full) {  // the window's canvas: at the window, its edge columns carried out to the glass's edges
          x0 = x0 == 0 ? 0 : kCartX + x0;
          x1 = x1 >= cw ? gw : kCartX + x1;
        }
        if (c.full && (y < kCartY || y >= kCartY + kCartPx)) {
          widen(c.edge_span0[y], c.edge_span1[y], c.edge_due[y >> 5] >> (y & 31) & 1, x0, x1);
          c.edge_due[y >> 5] |= 1u << (y & 31);
          continue;
        }
        const int r = (c.full ? y - kCartY : y) / kCartScale;
        widen(c.pic_span0[r], c.pic_span1[r], c.layer_due[r >> 5] >> (r & 31) & 1, x0, x1);
        c.layer_due[r >> 5] |= 1u << (r & 31);
      }
    }
    // A stage frame that starts too late for the lead its frames need goes whole behind the scan
    // (a pass later) rather than racing into a seam.
    c.tx_race_pass = race && (!c.stage || sc.start(show) - esp_timer_get_time() >= stage_need + kStageStartMarginUs) ? show : -1;
    c.tx_age = t.touching ? latch - t_us : -1;
    c.tx_t0 = latch;
    c.busy = true;
    xTaskNotifyGive(c.sender);
  }
  if (c.stage) {  // the next frame, for the pass after this one's, while the sender draws this one
    const int64_t p0 = esp_timer_get_time();
    if (c.overlay) {  // a 3D cart: its Lua moves the scene and draws the picture over it
      if (c.cart.running()) {
        const int64_t h0 = c.help_total.load();
        c.cart.frame(in);
        c.lua_us += c.cart.last().lua_us;
        // Lua's own: less the helper's drawing that preempted it on this core meanwhile
        c.lua_own_max = std::max(c.lua_own_max, c.cart.last().lua_us - (c.help_total.load() - h0));
        if (c.cart.last().lua_us > c.lua_max) c.lua_max_freed = c.cart.last().freed;
        c.lua_max = std::max(c.lua_max, c.cart.last().lua_us);
        if (!c.cart.error().empty()) ESP_LOGW(kTag, "cart %s stopped: %s", c.cart.name().c_str(), c.cart.error().c_str());
      }
    } else {
      if (t.touching && !c.edge) {
        c.drag_x = c.hold_x + static_cast<float>(t.x - c.down_x);
        c.drag_y = c.hold_y + static_cast<float>(t.y - c.down_y);
      } else {
        c.hold_x = c.drag_x;
        c.hold_y = c.drag_y;
      }
      const int64_t at = sc.ok ? sc.start(show + 1) : p0 + 16742;
      c.stage->step((at - c.stage_t0) / 1e6f, c.drag_x, c.drag_y);
    }
    const int64_t pp = esp_timer_get_time();
    c.stage->prepare();
    const int64_t prep = esp_timer_get_time() - pp, dt = esp_timer_get_time() - p0;
    c.prep_us += prep;
    c.prep_max = std::max(c.prep_max, prep);
    // The next frame's split. Halves draw each band soonest (the sender keeps pace with the scan
    // band by band); core 0 takes more only when core 1, which also prepares, would overrun:
    // its load is its preparing (less the helper's drawing meanwhile) plus its share of the drawing.
    if (c.helper) {
      constexpr float kCore1Budget = 13000;  // us a frame
      const float prep = static_cast<float>(std::max<int64_t>(dt - c.help_frame_acc.load(), 0));
      const float draw = static_cast<float>(c.own_frame + c.help_frame);
      if (draw > 0) {
        const float want = std::clamp(1.0f - (kCore1Budget - prep) / draw, 0.5f, 0.85f);
        c.share0 += (want - c.share0) * 0.2f;
        const int sw = c.stage->width();
        c.split = std::clamp(static_cast<int>(c.share0 * sw / 4 + 0.5f) * 4, 4, sw - 4);
      }
    }
    // res("auto"): half size while a frame's work runs over, full again when it would fit at full
    // size. Each core's work: core 1 its Lua and preparing plus its share of the drawing, core 0 its
    // bands (the scene, the picture, the layer); at half size the scene's drawing is about 3.5x less.
    if (c.stage->auto_res()) {
      const bool half = c.stage->wanted_scale() == 2;
      const float k = half ? 3.5f : 1.0f;
      const float own1 = static_cast<float>(std::max<int64_t>(dt - c.help_frame_acc.load(), 0));
      const float core1 = own1 + k * static_cast<float>(c.help_frame);
      const float core0 = static_cast<float>(c.raster_frame - c.own_frame) + k * static_cast<float>(c.own_frame);
      c.res_load += (std::max(core1, core0) - c.res_load) * 0.1f;
      const int64_t now = esp_timer_get_time();
      if (now - c.res_switched > 1000000 && (half ? c.res_load < 11000 : c.res_load > 14500)) {
        c.stage->set_scale(half ? 1 : 2);
        c.res_switched = now;
        ESP_LOGI(kTag, "stage %s: res auto: %s (a frame's work at full size %.1f ms)", c.stage_name.c_str(), half ? "full" : "half",
                 c.res_load / 1000.0);
      }
    }
    if (const int64_t need = c.need_lead.exchange(-1); need >= 0) c.lua_recent[c.lua_i++ % 16] = need;
  }
  cart_stats(c);
  return 1;
}

// A screenshot while a cart runs: the panel's picture, built from the cart's (the panel can't be
// read back).
bool cart_shot(uint16_t* px) {
  CartRun& c = *g_cart;
  const int w = dev->width(), h = dev->height();
  // One picture row as three panel rows, or a stage's band: PSRAM, briefly.
  auto* rows = static_cast<uint16_t*>(heap_caps_calloc(std::max(3 * w, kStageBand * carts::kStageW), 2, MALLOC_CAP_SPIRAM));
  if (!rows) return false;
  auto put = [&](const uint8_t* pic, int n, int nrows, int x, int y) {  // as cart_band sends it: full rows
    for (int r = 0; r < nrows; ++r) {
      cart_row(rows, w, x, pic, n, r, c);
      std::memcpy(px + static_cast<size_t>(y + r * kCartScale) * w, rows, static_cast<size_t>(w) * kCartScale * 2);
    }
  };
  if (c.stage) {  // the frame drawn again, band by band (the sender idle: the scene and depth rows are free)
    for (int i = 0; i < 50 && c.busy.load(); ++i) vTaskDelay(1);
    if (c.busy.load()) {
      heap_caps_free(rows);
      return false;
    }
    for (int y = 0; y < carts::Stage::kH; y += kStageBand) {
      c.stage->band(y, y + kStageBand, rows, c.depth);
      const int lx = (c.full ? 0 : kCartX) - c.stage_x, ly = (c.full ? kCartY : 0) + y;
      if (c.hd && !c.hd->over()) layer_rows(*c.hd, rows, c.stage->width(), lx, ly, kStageBand);
      if (c.overlay) carts::overlay_rows(rows, c.stage->width(), kCartX - c.stage_x, y, kStageBand, c.tx, c.pal, c.dim);
      if (c.hd && c.hd->over()) layer_rows(*c.hd, rows, c.stage->width(), lx, ly, kStageBand);
      for (int r = 0; r < kStageBand; ++r)
        std::memcpy(px + static_cast<size_t>(kCartY + y + r) * w + c.stage_x, rows + r * c.stage->width(), c.stage->width() * 2);
    }
  } else {
    put(c.cart.picture(), carts::kW, carts::kH, kCartX, kCartY);
  }
  if (c.full && c.hd)  // a full canvas: its rows above and below the window, as the sender's edge rows
    for (int y = 0; y < std::min(h, kGlassRows); ++y)
      if (y < kCartY || y >= kCartY + kCartPx) edge_row(px + static_cast<size_t>(y) * w, w, y, c);
  heap_caps_free(rows);
  return true;
}

void cart_relit() {
  CartRun& c = *g_cart;
  for (int i = 0; i < 50 && c.busy.load(); ++i) vTaskDelay(1);
  if (c.busy.load()) dev->timed_out("the cart sender (relighting)");
  dev->clear_glass();
  std::memset(c.shown, 0xFF, carts::kW * carts::kH);
  if (c.full) std::memset(c.edge_due, 0xFF, sizeof c.edge_due);  // a full canvas's rows around the window too
  std::memset(c.edge_span1, 0, sizeof c.edge_span1);  // all of each (a narrower change pending is now all of it)
  c.next_pass = -1;
}

}  // namespace

void begin(Device& device, store::Library& library, const Config& config) {
  dev = &device;
  g_lib = &library;
  g_cfg = config;
}
bool open(const std::string& name) { return cart_open(name); }
void close() { cart_close(); }
bool running() { return g_cart != nullptr; }
const std::string& name() { return g_open_name; }
uint32_t tick() { return g_cart ? cart_tick() : 200; }
bool closed_by_gesture() { return g_gesture_closed.exchange(false); }
bool slow() { return g_cart && g_slow.load(); }
bool stopped() { return g_cart && !g_cart->own_stage && !g_cart->cart.running(); }
const std::string& error() {
  static const std::string none;
  return g_cart ? g_cart->cart.error() : none;
}
void relit() {
  if (g_cart) cart_relit();
}
bool screenshot(uint16_t* px) { return g_cart && cart_shot(px); }
void set_button_b(bool down) { g_button_b = down; }

}  // namespace carts::player
