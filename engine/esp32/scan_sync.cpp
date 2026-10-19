#include "scan_sync.hpp"

#include <algorithm>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"


namespace carts::scan {
namespace {
const char* kTag = "hg.scan";
Config g_cfg;
// The GPIO interrupt service is shared by every driver that adds a pin handler (touch, keys, TE):
// installed by whoever comes first, and there for the rest.
bool gpio_isr_service_ready() {
  if (g_cfg.gpio_isr_service) return g_cfg.gpio_isr_service();
  const esp_err_t err = gpio_install_isr_service(0);
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;
}

}  // namespace

Te te;
Latency lat;

void configure(const Config& c) { g_cfg = c; }

namespace {
void IRAM_ATTR te_edge(void*) {
  const int64_t now = esp_timer_get_time();
  if (gpio_ll_get_level(&GPIO, static_cast<uint32_t>(te.pin))) {
    te.rise_us.store(now, std::memory_order_relaxed);
    return;
  }
  const int64_t last = te.fall_us.load(std::memory_order_relaxed), dt = now - last;
  if (last && dt > 12000 && dt < 25000) {  // a missed edge or a stall: not a period
    const int64_t p = te.period_us.load(std::memory_order_relaxed);
    te.period_us.store(p ? p + (dt - p) / 8 : dt, std::memory_order_relaxed);
  }
  const int64_t r = te.rise_us.load(std::memory_order_relaxed);
  if (r && now - r < 5000) te.high_us.store(now - r, std::memory_order_relaxed);
  te.fall_us.store(now, std::memory_order_relaxed);
  te.falls.fetch_add(1, std::memory_order_relaxed);
}
}  // namespace

void te_begin(int pin) {
  if (pin < 0) return;
  gpio_config_t io = {};
  io.pin_bit_mask = 1ULL << pin;
  io.mode = GPIO_MODE_INPUT;
  io.intr_type = GPIO_INTR_ANYEDGE;
  if (gpio_config(&io) != ESP_OK || !gpio_isr_service_ready() ||
      gpio_isr_handler_add(static_cast<gpio_num_t>(pin), &te_edge, nullptr) != ESP_OK) {
    ESP_LOGW(kTag, "no TE interrupt on GPIO %d: frames go out unsynchronised", pin);
    return;
  }
  te.pin = pin;
  te.armed = true;
}

// Off with the screen: a display-off panel may keep pulsing, and nothing needs it.
void te_arm(bool on) {
  if (te.pin < 0 || te.armed == on) return;
  te.armed = on;
  if (on) {
    te.fall_us = 0;
    te.period_us = 0;
    gpio_intr_enable(static_cast<gpio_num_t>(te.pin));
  } else {
    gpio_intr_disable(static_cast<gpio_num_t>(te.pin));
  }
}

namespace {
// A task's precise sleep: a one-shot timer and a semaphore it gives (made on the task's first long
// wait; a few tasks wait this way: the sender, the racer, the flusher).
struct Sleeper {
  TaskHandle_t task = nullptr;
  esp_timer_handle_t timer = nullptr;
  SemaphoreHandle_t woke = nullptr;
};
Sleeper* sleeper() {
  static Sleeper all[6];
  static StaticSemaphore_t lock_mem;
  static SemaphoreHandle_t lock = xSemaphoreCreateMutexStatic(&lock_mem);
  const TaskHandle_t me = xTaskGetCurrentTaskHandle();
  for (Sleeper& s : all)
    if (s.task == me) return &s;
  xSemaphoreTake(lock, portMAX_DELAY);
  Sleeper* got = nullptr;
  for (Sleeper& s : all) {
    if (s.task) continue;
    s.woke = xSemaphoreCreateBinary();
    esp_timer_create_args_t ta = {};
    ta.callback = [](void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); };
    ta.arg = s.woke;
    ta.name = "hg-sleep";
    if (s.woke && esp_timer_create(&ta, &s.timer) == ESP_OK) {
      s.task = me;
      got = &s;
    }
    break;
  }
  xSemaphoreGive(lock);
  return got;
}

}  // namespace

// Until time t, to the microsecond: most of the wait asleep on a one-shot timer (the core free for
// other tasks meanwhile: the network, the app), the last ~150 us spun (a timer's wake-up comes some
// tens of microseconds late). Spinning whole milliseconds at the sender's priority starved the
// app's own task on that core when a cart changed the whole glass every frame.
void wait_until(int64_t t) {
  constexpr int64_t kSpinUs = 150;
  int64_t left = t - esp_timer_get_time();
  if (left > kSpinUs + 100) {
    if (Sleeper* s = sleeper(); s && esp_timer_start_once(s->timer, static_cast<uint64_t>(left - kSpinUs)) == ESP_OK) {
      xSemaphoreTake(s->woke, portMAX_DELAY);
    } else {
      while ((left = t - esp_timer_get_time()) > 1500) vTaskDelay(1);
    }
  }
  left = t - esp_timer_get_time();
  if (left > 0) esp_rom_delay_us(static_cast<uint32_t>(left));
}

// Before a band of rows y0..y1 (glass rows) goes out. `first`: the frame's first band.
void te_gate(int y0, int y1, bool first) {
  if (te.pin < 0 || !te.on || !te.armed) return;
  const int64_t period = te.period_us.load();
  uint32_t base;
  int64_t fall;
  do {  // the count and its edge's time, from the same edge
    base = te.falls.load();
    fall = te.fall_us.load();
  } while (base != te.falls.load());
  int64_t now = esp_timer_get_time();
  if (!period || now - fall > 3 * period) return;  // no TE (yet): don't guess
  const int64_t active = period - std::min<int64_t>(te.high_us.load(), period / 4);
  const int rows = g_cfg.rows;
  const int shift = g_cfg.y_offset ? g_cfg.y_offset->load(std::memory_order_relaxed) : 0;
  y0 += shift;
  y1 += shift;
  auto at_row = [&](int y) { return static_cast<int64_t>(y) * active / rows; };
  // Passes counted from the last falling edge (missed edges are filled in by the period).
  auto pass_start = [&](int64_t n) { return fall + (n - base) * period; };
  const int64_t guard = at_row(te.guard_rows);
  const int64_t send = at_row(y1 - y0 + 1) * te.send_permille / 1000 + guard;  // the band's own transfer: faster than the scan
  auto opens = [&](int64_t n) { return pass_start(n) + at_row(y1) + guard; };       // that pass's scan is past it
  auto closes = [&](int64_t n) { return pass_start(n + 1) + at_row(y0) - send; };   // the next one reaches it
  if (te.race_pass >= 0 && !te.race_failed) {
    const int64_t m = te.race_pass;
    if (te.race_bands == 0) te.ahead_prep_max = std::max(te.ahead_prep_max, now - te.ahead_t0);
    if (now + send <= pass_start(m) + at_row(y0) - guard) {  // ahead of pass m's scan, with room
      const int64_t clear = pass_start(m - 1) + at_row(y1) + guard;  // and pass m - 1's is past it
      if (now < clear) wait_until(clear);
      te.frame_pass = te.last_pass = m - 1;  // shows on m, as a frame behind m - 1 does
      te.frame_open = true;
      ++te.race_bands;
      ++te.bands;
      return;
    }
    te.race_failed = true;
    if (te.race_bands == 0) {
      ++te.reader_fellback;  // the frame's first band: all of it the usual way, a pass later, no seam
    } else {
      ++te.ahead_late;  // mid-frame: this pass shows a seam
      ++te.ahead_late_band[std::min(15, y0 / g_cfg.band_rows)];
      first = true;     // pick the pass for the rest
      te.frame_open = false;
    }
  }
  int64_t n;
  if (first || !te.frame_open) {
    n = std::max<int64_t>(te.last_pass + 1, base + (now - fall) / period - 1);
    while (closes(n) < now) ++n;
    te.frame_pass = te.last_pass = n;
    te.frame_open = true;
  } else {
    n = te.frame_pass;
  }
  ++te.bands;
  if (now > closes(n)) {  // missed its pass (areas out of order, or a slow frame): the next one it can make
    ++te.late;
    while (closes(n) < now) ++n;
    te.frame_pass = te.last_pass = n;
  }
  const int64_t open = opens(n);
  if (now < open) {
    wait_until(open);
    const int64_t waited = esp_timer_get_time() - now;
    te.wait_us += waited;
    te.wait_total_us += waited;
  }
}
ScanClock scan_clock() {
  ScanClock c;
  c.period = te.period_us.load();
  do {
    c.base = te.falls.load();
    c.fall = te.fall_us.load();
  } while (c.base != te.falls.load());
  c.active = c.period - std::min<int64_t>(te.high_us.load(), c.period / 4);
  c.rows = g_cfg.rows;
  c.shift = g_cfg.y_offset ? g_cfg.y_offset->load(std::memory_order_relaxed) : 0;
  c.ok = te.pin >= 0 && te.on && te.armed && c.period > 0 && esp_timer_get_time() - c.fall < 3 * c.period;
  return c;
}
// Touch to photon for one frame: its bands went out behind pass `te.frame_pass`'s scan, so it
// shows on the next pass; mid-screen half its active scan later. Measured from the sample's time.
void lat_account(int64_t age, int64_t start_us, int64_t done_us) {
  if (age < 0 || age > 200000) return;  // no finger, or a stale sample (not a touch frame)
  const int64_t period = te.period_us.load();
  if (!te.on || !te.armed || period <= 0) return;
  uint32_t base;
  int64_t fall;
  do {
    base = te.falls.load();
    fall = te.fall_us.load();
  } while (base != te.falls.load());
  const int64_t active = period - std::min<int64_t>(te.high_us.load(), period / 4);
  const int64_t photon = fall + (te.frame_pass + 1 - static_cast<int64_t>(base)) * period + active / 2;
  lat.age += age;
  lat.work += done_us - start_us;
  lat.scan += photon - start_us;  // frame start to mid-screen showing it
  ++lat.n;
}

}  // namespace carts::scan
