#include "console.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "cart_player.hpp"
#include "cart_store.hpp"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "hd.hpp"
#include "hg/crypto.hpp"
#include "nvs.h"
#include "scan_sync.hpp"
#include "sound.hpp"
#include "stage.hpp"

// The launcher (engine/builtin/launcher.lua, embedded by the build).
extern "C" const char launcher_lua[] asm("_binary_launcher_lua_start");

namespace console {
namespace {

const char* TAG = "console";
using namespace carts::scan;
constexpr int kBandRows = carts::player::kBandRows;
constexpr const char* kLauncher = "launcher";
// Burn-in and battery: dim after a minute untouched, dark after ten (a touch, BOOT or PWR lights it).
constexpr uint32_t kDimMs = 60 * 1000, kDarkMs = 10 * 60 * 1000;
// The settings (the launcher's, or `volume N` / `brightness N` over USB), kept in NVS. Each is a
// step: volume off, low, medium, high (the codec's output level); brightness 25-100%. Dimmed is a
// third of the chosen brightness.
constexpr uint8_t kVolumes[] = {0, 50, 70, 90};
constexpr uint8_t kBrightness[] = {25, 50, 75, 100};
constexpr int kSteps = 4;
constexpr uint8_t kDefaultVolume = 1, kDefaultBrightness = 3;  // low, full: a new board starts quiet
constexpr UBaseType_t kIdlePrio = 4, kFramePrio = 6;  // the console task, and while a cart's frames run
constexpr uint32_t kBatteryEveryMs = 5000;
// The battery log (`battery log` over USB): a reading every 10 minutes, the last 96 hours. It's
// saved to NVS every hour and just before a low-battery power-off, and at the next start the saved
// one becomes `battery log last`: a battery-life run can be read after the board has died.
constexpr uint32_t kLogEveryMs = 10 * 60 * 1000;
constexpr size_t kLogSize = 576;
struct LogEntry {
  uint32_t minute;   // since start
  uint16_t mv;
  uint8_t percent;
  uint8_t flags;     // 1: the screen was lit, 2: on USB, 4: charging
};
constexpr size_t kLogSaveEvery = 6;  // entries: an hour
struct LogBlob {                     // the saved log's header, then its entries oldest first
  uint32_t magic = 0x31474f4c;       // "LOG1"
  int64_t start_unix = 0;            // the wall clock at the first entry (0: unknown then)
};
constexpr uint32_t kCutoffAfterMs = 60 * 1000;  // under the cut-off this long on battery: power off

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

struct Channel {
  std::string name;
  hg::json::Value value;
  uint32_t version;  // its own: a cart's data() table is made again only when its channel changed
};

struct State {
  Hardware hw{};
  uint16_t* bufs[2] = {};
  volatile bool busy[2] = {};
  SemaphoreHandle_t lock = nullptr;      // the player's: the console task, and everyone else's calls
  SemaphoreHandle_t band_sem = nullptr;  // given by a band's DMA completion
  TaskHandle_t task = nullptr, main_task = nullptr;
  portMUX_TYPE touch_mux = portMUX_INITIALIZER_UNLOCKED;
  board::TouchSample touch{false, 0, 0};
  int64_t touch_us = 0;
  std::atomic<int> down_x{0}, down_y{0};
  bool finger = false;              // touch task only
  volatile bool screen_on = true;   // what the panel shows; changed on the console task only
  std::atomic<bool> want_on{true};
  std::atomic<bool> swallow{false};  // the touch that woke the screen (or ended a swipe out of a cart) goes nowhere
  std::atomic<uint32_t> activity_ms{0};
  uint8_t level = 100;               // the panel's brightness now, percent
  uint8_t volume = kDefaultVolume, brightness = kDefaultBrightness;  // the settings, as steps
  std::atomic<bool> close_due{false}, open_due{false};
  std::string open_name;  // under the lock
  // Data channels for carts' data(): "carts" (the launcher's list), "battery" and "settings".
  SemaphoreHandle_t data_m = nullptr;
  std::vector<Channel> channels;
  uint32_t data_version = 1;  // any channel's
  // A message arriving over USB in parts, and the whole ones waiting for the worker (saving a
  // cart's images erases flash for seconds).
  std::string usb_msg;
  QueueHandle_t usb_queue = nullptr;  // hg::json::Value*
  // Power: no light sleep and full speed while lit.
  esp_pm_lock_handle_t awake = nullptr, full_speed = nullptr;
  esp_timer_handle_t sag_off = nullptr;  // the lit screen's voltage drop stops counting a minute after it goes dark
  uint32_t battery_at = 0, low_since = 0;
  LogEntry log[kLogSize] = {};
  size_t log_count = 0;  // all ever written; the newest kLogSize are kept
  uint32_t log_at = 0;
  int64_t log_start = 0;  // the wall clock at the first entry
  bool active = false;
};
State g;
carts::store::Library g_library;
void keep_last_log();  // (with the battery log, below)

struct Lock {
  Lock() { xSemaphoreTakeRecursive(g.lock, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(g.lock); }
};

void request_screen(bool on) {
  if (on) g.activity_ms = now_ms();
  g.want_on = on;
  if (g.task) xTaskNotifyGive(g.task);
}

// A channel's new value.
void set_channel(const std::string& name, hg::json::Value value) {
  xSemaphoreTake(g.data_m, portMAX_DELAY);
  const uint32_t ver = ++g.data_version;
  bool found = false;
  for (auto& c : g.channels)
    if (c.name == name) {
      c.value = std::move(value);
      c.version = ver;
      found = true;
      break;
    }
  if (!found) g.channels.push_back({name, std::move(value), ver});
  xSemaphoreGive(g.data_m);
}

// The carts list as the launcher reads it (data("carts")): [{name, title, about}, ...].
void publish_carts() {
  hg::json::Value list = hg::json::Value::array();
  for (const auto& e : g_library.list()) {
    hg::json::Value c = hg::json::Value::object();
    c.set("name", e.name).set("title", e.title).set("about", e.about);
    list.push(std::move(c));
  }
  set_channel("carts", std::move(list));
}

uint8_t full_level() { return kBrightness[g.brightness]; }
uint8_t dim_level() { return static_cast<uint8_t>(std::max(10, full_level() / 3)); }

// The settings as carts read them (data("settings")): {volume, brightness}, each a step 0-3.
void publish_settings() {
  hg::json::Value s = hg::json::Value::object();
  s.set("volume", static_cast<int>(g.volume)).set("brightness", static_cast<int>(g.brightness));
  set_channel("settings", std::move(s));
}

void save_setting(const char* key, uint8_t value) {
  nvs_handle_t h;
  if (nvs_open("luanette", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "settings: NVS won't open; %s isn't kept", key);
    return;
  }
  if (nvs_set_u8(h, key, value) != ESP_OK || nvs_commit(h) != ESP_OK) ESP_LOGW(TAG, "settings: %s isn't kept", key);
  nvs_close(h);
}

void load_settings() {
  nvs_handle_t h;
  if (nvs_open("luanette", NVS_READONLY, &h) != ESP_OK) return;  // none saved yet: the defaults
  uint8_t v;
  if (nvs_get_u8(h, "volume", &v) == ESP_OK && v < kSteps) g.volume = v;
  if (nvs_get_u8(h, "brightness", &v) == ESP_OK && v < kSteps) g.brightness = v;
  nvs_close(h);
}

void apply_volume() {
  if (g.hw.speaker) g.hw.speaker->set_volume(kVolumes[g.volume]);
  carts::sound::set_muted(g.volume == 0);
}

// A setting changed (the launcher's act(), or USB). False: not a step.
bool set_volume(int step) {
  if (step < 0 || step >= kSteps) return false;
  g.volume = static_cast<uint8_t>(step);
  apply_volume();
  save_setting("volume", g.volume);
  publish_settings();
  return true;
}

bool set_brightness(int step) {
  if (step < 0 || step >= kSteps) return false;
  g.brightness = static_cast<uint8_t>(step);
  if (g.screen_on) {  // lit (or dimmed) now: the new level at once
    g.level = full_level();
    g.hw.panel->set_brightness(g.level);
    g.activity_ms = now_ms();
  }
  save_setting("brightness", g.brightness);
  publish_settings();
  return true;
}

void IRAM_ATTR band_done(void* ctx) {
  g.busy[reinterpret_cast<uintptr_t>(ctx)] = false;
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(g.band_sem, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void timed_out(const char* what) {
  static uint32_t last = 0, quiet = 0;
  const uint32_t now = now_ms();
  if (now - last < 200) {
    ++quiet;
    return;
  }
  last = now;
  ESP_LOGW(TAG, "%s: timed out, carrying on (%u not logged)", what, static_cast<unsigned>(quiet));
  quiet = 0;
}

class Device final : public carts::player::Device {
 public:
  int width() const override { return g.hw.panel->config().width; }
  int height() const override { return g.hw.panel->config().height; }
  int corner_radius() const override { return g.hw.panel->config().corner_radius; }
  bool round() const override { return g.hw.panel->config().round; }
  void clear_glass() override { g.hw.panel->clear_glass(0, 0, width(), height()); }
  uint16_t* band(int k) override { return g.bufs[k]; }
  void band_wait(int k) override {
    for (int i = 0; i < 25 && g.busy[k]; ++i) xSemaphoreTake(g.band_sem, pdMS_TO_TICKS(10));
    if (g.busy[k]) {
      timed_out("a band's transfer");
      g.busy[k] = false;
    }
  }
  bool band_send(int k, int x, int y, int w, int h) override {
    g.busy[k] = true;
    if (g.hw.panel->write_rows(x, y, w, h, g.bufs[k], &band_done, reinterpret_cast<void*>(uintptr_t(k)))) return true;
    g.busy[k] = false;
    return false;
  }
  bool screen_on() const override { return g.screen_on; }
  void wake_screen() override { request_screen(true); }
  carts::player::Touch touch() override {
    carts::player::Touch t;
    portENTER_CRITICAL(&g.touch_mux);
    t.touching = g.touch.touching;
    t.x = g.touch.x;
    t.y = g.touch.y;
    t.t_us = g.touch_us;
    portEXIT_CRITICAL(&g.touch_mux);
    t.down_x = g.down_x.load();
    t.down_y = g.down_y.load();
    return t;
  }
  void note_activity() override { g.activity_ms = now_ms(); }
  bool has_imu() const override { return g.hw.imu != nullptr; }
  bool imu_enable(bool on) override { return g.hw.imu && g.hw.imu->enable(on); }
  bool imu_read(float* x, float* y, float* z) override { return g.hw.imu && g.hw.imu->read(x, y, z); }
  // The console task (the player's frames). A copy of the channels, taken when they change.
  const hg::json::Value* data(std::string_view name, uint32_t* version) override {
    xSemaphoreTake(g.data_m, portMAX_DELAY);
    if (copy_version_ != g.data_version) {
      copy_ = g.channels;
      copy_version_ = g.data_version;
    }
    xSemaphoreGive(g.data_m);
    for (const auto& c : copy_)
      if (c.name == name) {
        *version = c.version;
        return &c.value;
      }
    return nullptr;
  }
  // The launcher's act(): "open" a cart (at the top of the next loop, not from inside a frame),
  // and the settings. There's no host to take anything else.
  bool accepts_command(std::string_view name) const override {
    return name == "open" || name == "volume" || name == "brightness";
  }
  void command(std::string_view name, std::string_view arg) override {
    if (name == "open") {
      g.open_name = std::string(arg);
      g.open_due = true;
    } else if (name == "volume" || name == "brightness") {
      const int step = std::atoi(std::string(arg).c_str());
      if (!(name == "volume" ? set_volume(step) : set_brightness(step)))
        ESP_LOGW(TAG, "act(%.*s, %.*s): not a step (0-%d)", static_cast<int>(name.size()), name.data(),
                 static_cast<int>(arg.size()), arg.data(), kSteps - 1);
    } else if (name != "cart" && name != "image") {
      ESP_LOGI(TAG, "act(%.*s): no host to take it", static_cast<int>(name.size()), name.data());
    }
  }
  void timed_out(const char* what) override { console::timed_out(what); }

 private:
  std::vector<Channel> copy_;
  uint32_t copy_version_ = 0;
};
Device g_device;

// The screen's power. Dark: display off and the panel asleep, touch and the keys armed as wake
// sources, and light sleep allowed. Lit: the reverse, at full speed.
void apply_screen(bool on) {
  if (!on) {
    g.screen_on = false;
    g.hw.panel->set_brightness(0);  // waits for any band still queued
    te_arm(false);
    if (g.hw.touch) g.hw.touch->set_wake(true);
    if (g.hw.buttons) g.hw.buttons->set_wake(true, g.main_task);
    if (g.sag_off) esp_timer_start_once(g.sag_off, 60 * 1000 * 1000);
    if (g.full_speed) esp_pm_lock_release(g.full_speed);
    if (g.awake) esp_pm_lock_release(g.awake);
    ESP_LOGI(TAG, "dark");
  } else {
    if (g.awake) esp_pm_lock_acquire(g.awake);
    if (g.full_speed) esp_pm_lock_acquire(g.full_speed);
    if (g.sag_off) esp_timer_stop(g.sag_off);
    if (g.hw.power) g.hw.power->set_sag_mv(board::kConfig.lit_sag_mv);
    if (g.hw.touch) g.hw.touch->set_wake(false);
    if (g.hw.buttons) g.hw.buttons->set_wake(false, g.main_task);
    g.hw.panel->wake();
    te_arm(true);
    g.screen_on = true;
    carts::player::relit();  // everything again, before it lights (the old picture never shows)
    g.level = full_level();
    g.hw.panel->set_brightness(g.level);
  }
}

void idle_policy() {
  const uint32_t idle = now_ms() - g.activity_ms.load();
  if (idle >= kDarkMs) {
    request_screen(false);
    return;
  }
  const uint8_t want = idle >= kDimMs ? dim_level() : full_level();
  if (want != g.level) {
    g.level = want;
    g.hw.panel->set_brightness(want);
  }
}

void console_task(void*) {
  uint32_t relaunch_at = 0;
  for (;;) {
    uint32_t wait = 50;
    {
      Lock l;
      const bool want = g.want_on.load();
      if (want != g.screen_on) apply_screen(want);
      if (g.screen_on) idle_policy();
      if (g.close_due.exchange(false) && carts::player::running()) carts::player::close();
      if (g.open_due.exchange(false) && !carts::player::open(g.open_name)) ESP_LOGW(TAG, "can't open %s", g.open_name.c_str());
      // The launcher stopped (an error in it): it goes, and starts again in a moment.
      if (carts::player::running() && carts::player::name() == kLauncher && carts::player::stopped()) {
        ESP_LOGE(TAG, "the launcher stopped: %s; starting it again", carts::player::error().c_str());
        carts::player::close();
        relaunch_at = now_ms() + 2000;
      }
      if (!carts::player::running() && static_cast<int32_t>(now_ms() - relaunch_at) >= 0 && !carts::player::open(kLauncher)) {
        ESP_LOGE(TAG, "the launcher won't start");
        relaunch_at = now_ms() + 2000;
      }
      if (carts::player::running()) wait = carts::player::tick();
      if (carts::player::closed_by_gesture()) {  // the rest of that swipe belongs to nobody, the launcher included
        g.swallow = true;
        portENTER_CRITICAL(&g.touch_mux);
        g.touch.touching = false;
        portEXIT_CRITICAL(&g.touch_mux);
      }
    }
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g.screen_on ? std::clamp<uint32_t>(wait, 1, 16) : 1000));
  }
}

void* band_buffer(size_t bytes) {
  void* b = heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  return b ? b : heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_DMA);  // PSRAM-backed, slower
}

// The worker that takes whole USB messages, in the order they came.
void usb_worker(void*) {
  for (hg::json::Value* m = nullptr;;) {
    if (xQueueReceive(g.usb_queue, &m, portMAX_DELAY) != pdTRUE) continue;
    on_message(*m);
    delete m;
  }
}

std::string status_json() {
  hg::json::Value s = hg::json::Value::object();
  s.set("firmware", esp_app_get_description()->version).set("board", board::kConfig.name);
  if (g.hw.power)
    if (const auto p = g.hw.power->read()) {
      if (p->battery_mv) s.set("battery_mv", static_cast<int>(*p->battery_mv));
      if (p->battery_percent) s.set("battery_percent", static_cast<int>(*p->battery_percent));
      if (p->charging) s.set("charging", *p->charging);
      if (p->external_power) s.set("usb", *p->external_power);
    }
  s.set("cart", carts::player::name()).set("screen", g.screen_on ? "on" : "off");
  s.set("volume", static_cast<int>(g.volume)).set("brightness", static_cast<int>(g.brightness));
  s.set("uptime_s", static_cast<double>(esp_timer_get_time() / 1000000));
  s.set("internal_free", static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
  s.set("psram_free", static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  const std::time_t now = std::time(nullptr);
  if (now > 1700000000) s.set("time", static_cast<double>(now));
  return s.dump();
}

}  // namespace

bool begin(const Hardware& hw) {
  g.hw = hw;
  g.main_task = xTaskGetCurrentTaskHandle();
  const auto& pc = hw.panel->config();
  carts::scan::configure({static_cast<int>(pc.height), nullptr, kBandRows, &board::gpio_isr_service});
  te_begin(pc.te);
  g.lock = xSemaphoreCreateRecursiveMutex();
  g.band_sem = xSemaphoreCreateBinary();
  g.data_m = xSemaphoreCreateMutex();
  g.usb_queue = xQueueCreate(8, sizeof(hg::json::Value*));
  const size_t band = static_cast<size_t>(pc.width) * kBandRows * 2;
  g.bufs[0] = static_cast<uint16_t*>(band_buffer(band));
  g.bufs[1] = static_cast<uint16_t*>(band_buffer(band));
  if (!g.lock || !g.band_sem || !g.data_m || !g.usb_queue || !g.bufs[0] || !g.bufs[1]) {
    ESP_LOGE(TAG, "no memory for the console");
    return false;
  }
  hw.panel->set_deep_sleep(true);  // dark: the panel's converters off too
  g_library.load();
  g_library.add_hidden(kLauncher, launcher_lua);
  carts::player::begin(g_device, g_library, {kIdlePrio, kFramePrio});
  publish_carts();
  load_settings();
  keep_last_log();
  apply_volume();
  g.level = full_level();
  hw.panel->set_brightness(g.level);
  publish_settings();
  // Power: held while lit; dark, they go and the chip light-sleeps between events.
  if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "console-lit", &g.awake) == ESP_OK) esp_pm_lock_acquire(g.awake);
  if (esp_pm_lock_create(ESP_PM_CPU_FREQ_MAX, 0, "console-fast", &g.full_speed) == ESP_OK) esp_pm_lock_acquire(g.full_speed);
  if (hw.power) {
    hw.power->set_sag_mv(board::kConfig.lit_sag_mv);
    esp_timer_create_args_t ta = {};
    ta.callback = [](void*) { g.hw.power->set_sag_mv(0); };
    ta.name = "console-sag";
    esp_timer_create(&ta, &g.sag_off);
  }
  g.activity_ms = now_ms();
  if (xTaskCreatePinnedToCore(console_task, "console", 16384, nullptr, kIdlePrio, &g.task, 1) != pdPASS ||
      xTaskCreate(usb_worker, "console-usb", 6144, nullptr, 3, nullptr) != pdPASS) {
    ESP_LOGE(TAG, "no console task");
    return false;
  }
  g.active = true;
  ESP_LOGI(TAG, "console on %ux%u", pc.width, pc.height);
  return true;
}

void on_touch(const board::TouchSample& s) {
  const int64_t now = esp_timer_get_time();
  if (s.touching && !g.finger) {
    g.down_x = s.x;
    g.down_y = s.y;
  }
  g.finger = s.touching;
  if (!g.want_on.load()) {  // dark: a touch wakes it and goes nowhere else
    if (s.touching) {
      g.swallow = true;
      request_screen(true);
    }
    return;
  }
  g.activity_ms = now_ms();
  if (g.swallow) {  // until that finger lifts; meanwhile the cart sees no finger
    if (!s.touching) g.swallow = false;
    portENTER_CRITICAL(&g.touch_mux);
    g.touch = {false, s.x, s.y};
    g.touch_us = now;
    portEXIT_CRITICAL(&g.touch_mux);
    return;
  }
  portENTER_CRITICAL(&g.touch_mux);
  g.touch = s;
  g.touch_us = now;
  portEXIT_CRITICAL(&g.touch_mux);
  if (g.task) xTaskNotifyGive(g.task);  // a lazy cart's frame comes forward for it
}

void on_boot(bool pressed) {
  carts::player::set_button_b(pressed);
  g.activity_ms = now_ms();
  if (pressed && !g.want_on.load()) request_screen(true);
}

void on_pwr() {
  g.activity_ms = now_ms();
  if (!g.want_on.load()) {
    request_screen(true);
    return;
  }
  Lock l;
  if (carts::player::running() && carts::player::name() != kLauncher) {
    g.close_due = true;  // back to the launcher
    if (g.task) xTaskNotifyGive(g.task);
  } else {
    request_screen(false);
  }
}

namespace {

// The log, oldest first, as `battery log` gives it: {"start": unix, "log": [[minute, mV, percent, flags], ...]}.
std::string log_json(int64_t start, const LogEntry* entries, size_t n) {
  std::string out = "{\"start\":" + std::to_string(start) + ",\"log\":[";
  for (size_t k = 0; k < n; ++k) {
    const LogEntry& e = entries[k];
    out += (k ? ",[" : "[") + std::to_string(e.minute) + "," + std::to_string(e.mv) + "," + std::to_string(e.percent) + "," +
           std::to_string(e.flags) + "]";
  }
  return out + "]}";
}

std::vector<LogEntry> log_entries() {
  const size_t n = std::min(g.log_count, kLogSize);
  std::vector<LogEntry> out(n);
  for (size_t k = 0; k < n; ++k) out[k] = g.log[(g.log_count - n + k) % kLogSize];
  return out;
}

void save_log() {
  const std::vector<LogEntry> entries = log_entries();
  LogBlob head;
  head.start_unix = g.log_start;
  std::vector<uint8_t> blob(sizeof head + entries.size() * sizeof(LogEntry));
  std::memcpy(blob.data(), &head, sizeof head);
  if (!entries.empty()) std::memcpy(blob.data() + sizeof head, entries.data(), entries.size() * sizeof(LogEntry));
  nvs_handle_t h;
  if (nvs_open("luanette", NVS_READWRITE, &h) != ESP_OK) {
    ESP_LOGW(TAG, "battery log: NVS won't open; not saved");
    return;
  }
  if (nvs_set_blob(h, "battlog", blob.data(), blob.size()) != ESP_OK || nvs_commit(h) != ESP_OK)
    ESP_LOGW(TAG, "battery log: not saved");
  nvs_close(h);
}

// At start: the log saved by the last run becomes "battlog_prev" (`battery log last`).
void keep_last_log() {
  nvs_handle_t h;
  if (nvs_open("luanette", NVS_READWRITE, &h) != ESP_OK) return;
  size_t len = 0;
  if (nvs_get_blob(h, "battlog", nullptr, &len) == ESP_OK && len >= sizeof(LogBlob)) {
    std::vector<uint8_t> blob(len);
    if (nvs_get_blob(h, "battlog", blob.data(), &len) == ESP_OK && nvs_set_blob(h, "battlog_prev", blob.data(), len) == ESP_OK)
      nvs_erase_key(h, "battlog");
    nvs_commit(h);
  }
  nvs_close(h);
}

std::string last_log_json() {
  nvs_handle_t h;
  if (nvs_open("luanette", NVS_READONLY, &h) != ESP_OK) return "";
  size_t len = 0;
  std::vector<uint8_t> blob;
  if (nvs_get_blob(h, "battlog_prev", nullptr, &len) == ESP_OK && len >= sizeof(LogBlob)) {
    blob.resize(len);
    if (nvs_get_blob(h, "battlog_prev", blob.data(), &len) != ESP_OK) blob.clear();
  }
  nvs_close(h);
  LogBlob head;
  if (blob.size() < sizeof head) return "";
  std::memcpy(&head, blob.data(), sizeof head);
  if (head.magic != LogBlob{}.magic) return "";
  const size_t n = (blob.size() - sizeof head) / sizeof(LogEntry);
  std::vector<LogEntry> entries(n);
  if (n) std::memcpy(entries.data(), blob.data() + sizeof head, n * sizeof(LogEntry));
  return log_json(head.start_unix, entries.data(), n);
}

}  // namespace

// The battery every few seconds (a "battery" channel for carts), and the low-battery cut-off: on
// battery under it for a minute, a clean power-off ahead of the brownout.
void tick_main() {
  if (!g.active || !g.hw.power || now_ms() - g.battery_at < kBatteryEveryMs) return;
  g.battery_at = now_ms();
  const auto p = g.hw.power->read();
  if (!p) return;
  hg::json::Value b = hg::json::Value::object();
  if (p->battery_mv) b.set("mv", static_cast<int>(*p->battery_mv));
  if (p->battery_percent) b.set("percent", static_cast<int>(*p->battery_percent));
  b.set("charging", p->charging.value_or(false)).set("usb", p->external_power.value_or(false));
  set_channel("battery", std::move(b));
  if (!g.log_count || now_ms() - g.log_at >= kLogEveryMs) {
    g.log_at = now_ms();
    if (!g.log_count) {
      const std::time_t t = std::time(nullptr);
      g.log_start = t > 1700000000 ? static_cast<int64_t>(t) : 0;
    }
    g.log[g.log_count++ % kLogSize] = {static_cast<uint32_t>(esp_timer_get_time() / 60000000),
                                       static_cast<uint16_t>(p->battery_mv.value_or(0)),
                                       static_cast<uint8_t>(p->battery_percent.value_or(0)),
                                       static_cast<uint8_t>((g.screen_on ? 1 : 0) | (p->external_power.value_or(false) ? 2 : 0) |
                                                            (p->charging.value_or(false) ? 4 : 0))};
    if (g.log_count % kLogSaveEvery == 0) save_log();
  }
  const bool low = p->battery_mv && *p->battery_mv < board::kConfig.cutoff_mv && !p->external_power.value_or(false);
  if (!low) {
    g.low_since = 0;
  } else if (!g.low_since) {
    g.low_since = now_ms() ? now_ms() : 1;
  } else if (now_ms() - g.low_since >= kCutoffAfterMs) {
    ESP_LOGW(TAG, "battery %u mV, under %u mV for a minute: powering off", static_cast<unsigned>(*p->battery_mv),
             static_cast<unsigned>(board::kConfig.cutoff_mv));
    save_log();  // the run's log survives the power-off (`battery log last` after the next start)
    vTaskDelay(pdMS_TO_TICKS(200));  // the log line out
    g.hw.power->power_off();
  }
}

bool sleepy() { return !g.screen_on && (!g.hw.buttons || g.hw.buttons->idle()); }

bool on_message(const hg::json::Value& msg) {
  if (!g.active) return false;
  const std::string_view type = msg["type"].str_or("");
  if (type == "cart" || type == "cart.images" || type == "cart.image") {
    carts::store::Library::Outcome o;
    g_library.on_message(msg, o);
    if (o.changed) publish_carts();
    Lock l;
    if (o.close) g.close_due = true;
    if (!o.deleted.empty() && carts::player::name() == o.deleted) g.close_due = true;  // removed while open: it stops
    if (!o.open.empty()) {
      g.open_name = o.open;
      g.open_due = true;
      request_screen(true);
    }
    if (g.task) xTaskNotifyGive(g.task);
    return true;
  }
  return false;
}

// The USB link's lines (tools/push.py speaks this). Carts as the host's messages in parts:
// `cart msg begin`, `cart msg <offset> <crc32> <base64>` for each part (answered "@ok <bytes so
// far>", or "@error at <bytes so far>" for a damaged or misplaced one: the host sends again from
// there), then `cart msg end <crc32 of the whole>`.
std::string on_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
  if (line == "status") return "@status " + status_json();
  if (line.rfind("volume ", 0) == 0 || line.rfind("brightness ", 0) == 0) {  // a setting, 0-3
    const bool vol = line[0] == 'v';
    const int step = std::atoi(std::string(line.substr(vol ? 7 : 11)).c_str());
    Lock l;
    return (vol ? set_volume(step) : set_brightness(step)) ? "@ok" : "@error a step: 0, 1, 2 or 3";
  }
#if CONFIG_PM_PROFILING
  if (line == "pm") {  // the power locks and the time in each mode (a diagnostic build only)
    char* buf = nullptr;
    size_t len = 0;
    FILE* f = open_memstream(&buf, &len);
    if (!f) return "@error no memory";
    esp_pm_dump_locks(f);
    std::fclose(f);
    std::string out(buf, len);
    std::free(buf);
    for (char& ch : out)
      if (ch == '\n') ch = '|';
    return "@pm " + out;
  }
#endif
  if (line == "battery log") {  // this run's
    const std::vector<LogEntry> e = log_entries();
    return "@battery " + log_json(g.log_start, e.data(), e.size());
  }
  if (line == "battery log save") {  // saved now, as every hour (for testing the saved log)
    save_log();
    return "@ok";
  }
  if (line == "battery log last") {  // the run before the last start, saved in NVS
    const std::string j = last_log_json();
    return j.empty() ? "@error no saved battery log" : "@battery " + j;
  }
  if (line.rfind("time ", 0) == 0) {  // `time <unix seconds>`: the clock, and the RTC
    const long long t = std::atoll(std::string(line.substr(5)).c_str());
    return t > 1700000000 && board::clock::set(static_cast<std::time_t>(t)) ? "@ok" : "@error time <unix seconds>";
  }
  if (line.rfind("tz ", 0) == 0) {  // `tz <POSIX TZ>`: local time for date()
    board::clock::set_tz(std::string(line.substr(3)).c_str());
    return "@ok";
  }
  if (line == "screen on" || line == "screen off") {  // as PWR would: the screen, and light sleep while it's dark
    g.activity_ms = now_ms();
    request_screen(line == "screen on");
    return "@ok";
  }
  if (line.rfind("check panel", 0) == 0) {  // `check panel [frames]`: the sender's pixels against the frame composed afresh
    const int n = line.size() > 12 ? std::atoi(std::string(line.substr(12)).c_str()) : 60;
    carts::player::tuning.panel_check = std::clamp(n, 1, 1000);
    return "@ok the result goes to the log (\"panel check\")";
  }
  if (line == "check selftest") {  // the vector (PIE) loops against their C versions
    std::string why;
    const int bad = carts::hd_pie_selftest(&why);
    const std::string stage = carts::Stage::selftest();
    return std::string("@ok layer: ") + (bad == 0 ? "bit for bit" : bad < 0 ? "no vector loops" : "MISMATCHES: " + why) +
           "; stage: " + stage;
  }
  // A screenshot: the glass as the cart shows it, RGB565 big-endian, kept until the next one.
  // `shot` takes it and sends every part as `@shot <i> <crc32> <base64>`, then `@ok <w> <h> <parts>`;
  // `shot <i>` sends part i again (the log shares the port: a line can come back damaged).
  if (line == "shot" || line.rfind("shot ", 0) == 0) {
    static std::vector<uint8_t> shot;  // PSRAM (a large allocation)
    constexpr size_t kChunk = 3 * 1024;
    const auto& pc = g.hw.panel->config();
    if (line == "shot") {
      const size_t n = static_cast<size_t>(pc.width) * pc.height;
      shot.assign(n * 2, 0);
      bool ok;
      {
        Lock l;
        ok = carts::player::screenshot(reinterpret_cast<uint16_t*>(shot.data()));
      }
      if (!ok) return "@error busy; try again";
    }
    if (shot.empty()) return "@error no screenshot taken (send `shot` first)";
    const size_t parts = (shot.size() + kChunk - 1) / kChunk;
    auto part = [&](size_t i) {
      const size_t off = i * kChunk, len = std::min(kChunk, shot.size() - off);
      char crc[12];
      std::snprintf(crc, sizeof crc, "%08lx", static_cast<unsigned long>(esp_rom_crc32_le(0, shot.data() + off, len)));
      return "@shot " + std::to_string(i) + " " + crc + " " + hg::crypto::base64_encode(shot.data() + off, len) + "\n";
    };
    std::string out;
    if (line == "shot") {
      for (size_t i = 0; i < parts; ++i) out += part(i);
    } else {
      const size_t i = static_cast<size_t>(std::atoi(std::string(line.substr(5)).c_str()));
      if (i >= parts) return "@error no such part";
      out = part(i);
    }
    return out + "@ok " + std::to_string(pc.width) + " " + std::to_string(pc.height) + " " + std::to_string(parts);
  }
  if (line.rfind("cart ", 0) != 0)
    return "@error commands: status | battery log [last|save] | volume N | brightness N | shot | screen on|off | time <unix> | tz <posix> | check panel [n] | check selftest | cart ...";
  std::string_view rest = line.substr(5);
  if (rest == "msg begin") {
    g.usb_msg.clear();
    return "@ok 0";
  }
  if (rest.rfind("msg end", 0) == 0) {
    const uint32_t want = static_cast<uint32_t>(std::strtoul(std::string(rest.substr(7)).c_str(), nullptr, 16));
    const uint32_t got = esp_rom_crc32_le(0, reinterpret_cast<const uint8_t*>(g.usb_msg.data()), g.usb_msg.size());
    auto* msg = new hg::json::Value();
    const bool whole = got == want;
    const bool ok = whole && hg::json::parse(g.usb_msg, *msg) && msg->is_object();
    const size_t n = g.usb_msg.size();
    g.usb_msg.clear();
    g.usb_msg.shrink_to_fit();
    if (!ok) {
      delete msg;
      return whole ? "@error not a JSON message" : "@error message damaged (" + std::to_string(n) + " bytes)";
    }
    if (xQueueSend(g.usb_queue, &msg, pdMS_TO_TICKS(2500)) != pdTRUE) {
      delete msg;
      return "@error busy saving; send it again";
    }
    return "@ok";
  }
  if (rest.rfind("msg ", 0) == 0) {
    const std::string at = "@error at " + std::to_string(g.usb_msg.size());
    std::string_view body = rest.substr(4);
    const size_t s1 = body.find(' '), s2 = s1 == std::string_view::npos ? s1 : body.find(' ', s1 + 1);
    if (s2 == std::string_view::npos) return at;
    const unsigned long off = std::strtoul(std::string(body.substr(0, s1)).c_str(), nullptr, 16);
    const uint32_t want = static_cast<uint32_t>(std::strtoul(std::string(body.substr(s1 + 1, s2 - s1 - 1)).c_str(), nullptr, 16));
    if (off != g.usb_msg.size()) return at;  // a part after a damaged one: the host goes back
    std::vector<uint8_t> part;
    if (!hg::crypto::base64_decode(std::string(body.substr(s2 + 1)), part) || esp_rom_crc32_le(0, part.data(), part.size()) != want)
      return at;
    g.usb_msg.append(reinterpret_cast<const char*>(part.data()), part.size());
    return "@ok " + std::to_string(g.usb_msg.size());
  }
  if (rest == "list") {
    hg::json::Value list = hg::json::Value::array();
    for (const auto& e : g_library.list()) list.push(hg::json::Value(e.name));
    return "@carts " + list.dump();
  }
  if (rest.rfind("open ", 0) == 0) {
    Lock l;
    g.open_name = std::string(rest.substr(5));
    g.open_due = true;
    request_screen(true);
    return "@ok";
  }
  if (rest == "close") {
    g.close_due = true;
    if (g.task) xTaskNotifyGive(g.task);
    return "@ok";
  }
  return "@error cart commands: msg begin | msg <offset> <crc32> <base64> | msg end <crc32> | list | open <name> | close";
}

}  // namespace console
