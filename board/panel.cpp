// The CO5300 AMOLED controller over QSPI, through esp_lcd's panel IO in quad mode.
//
// Framing on the QSPI link: commands go out as (0x02 << 24) | (cmd << 8) with their parameters;
// pixels as (0x32 << 24) | (RAMWR << 8). The controller only takes windows that start on an even
// row and column and span an even count.
#include "board.hpp"

#include <algorithm>
#include <cstring>

#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

namespace board {
namespace {

const char* TAG = "board.panel";
constexpr spi_host_device_t kHost = SPI2_HOST;

constexpr uint32_t command_word(uint8_t cmd) { return (0x02u << 24) | (static_cast<uint32_t>(cmd) << 8); }
constexpr uint32_t kRamWrite = (0x32u << 24) | (0x2Cu << 8);

struct InitCommand {
  uint8_t cmd;
  uint8_t data[4];
  uint8_t len;
  uint16_t delay_ms;
};

// Bring-up for the 410x502 module, following Waveshare's esp32_s3_touch_amoled_2_06 BSP 2.0.0:
// vendor page, RGB565, the tearing line on, full brightness, the window (column offset 22), sleep
// out and display on.
constexpr InitCommand kInit[] = {
    {0xFE, {0x00}, 1, 0},
    {0xC4, {0x80}, 1, 0},
    {0x3A, {0x55}, 1, 0},  // 16 bits per pixel
    {0x35, {0x00}, 1, 0},  // tearing effect line on
    {0x53, {0x20}, 1, 0},  // brightness control on
    {0x51, {0xFF}, 1, 0},  // brightness
    {0x63, {0xFF}, 1, 0},
    {0x2A, {0x00, 0x16, 0x01, 0xAF}, 4, 0},
    {0x2B, {0x00, 0x00, 0x01, 0xF5}, 4, 0},
    {0x11, {}, 0, 120},  // sleep out
    {0x29, {}, 0, 0},    // display on
};

// The panel IO isn't safe for two tasks at once (esp_lcd's SPI IO counts its own transfers in
// flight), and more than one task sends: every transaction takes this lock. Recursive:
// write_rows -> set_window -> command.
struct IoLock {
  explicit IoLock(SemaphoreHandle_t m) : m_(m) {
    if (m_) xSemaphoreTakeRecursive(m_, portMAX_DELAY);
  }
  ~IoLock() {
    if (m_) xSemaphoreGiveRecursive(m_);
  }
  SemaphoreHandle_t m_;
};

bool ok(esp_err_t err, const char* what) {
  if (err == ESP_OK) return true;
  ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
  return false;
}

}  // namespace

bool Panel::on_trans_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void* ctx) {
  auto* self = static_cast<Panel*>(ctx);
  if (self->pending_head_ != self->pending_tail_) {  // a write_rows: its caller's completion
    const Pending p = self->pending_[self->pending_head_ % kPending];
    self->pending_head_ = self->pending_head_ + 1;
    if (p.cb) p.cb(p.ctx);
    return false;
  }
  BaseType_t woken = pdFALSE;  // a clear's own transfer
  xSemaphoreGiveFromISR(self->done_, &woken);
  if (woken) portYIELD_FROM_ISR();
  return false;
}

void Panel::command(uint8_t cmd, const uint8_t* data, size_t len) {
  IoLock l(io_lock_);
  esp_lcd_panel_io_tx_param(io_, static_cast<int>(command_word(cmd)), len ? data : nullptr, len);
}

void Panel::set_window(int x0, int y0, int x1, int y1) {
  const uint8_t cols[4] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0), static_cast<uint8_t>(x1 >> 8),
                           static_cast<uint8_t>(x1)};
  const uint8_t lines[4] = {static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0), static_cast<uint8_t>(y1 >> 8),
                            static_cast<uint8_t>(y1)};
  command(0x2A, cols, 4);
  command(0x2B, lines, 4);
}

bool Panel::begin(const PanelConfig& cfg, int max_rows) {
  cfg_ = cfg;
  io_lock_ = xSemaphoreCreateRecursiveMutex();
  done_ = xSemaphoreCreateBinary();
  if (!io_lock_ || !done_) return false;

  spi_bus_config_t bus = {};
  bus.sclk_io_num = cfg.sclk;
  bus.data0_io_num = cfg.d0;
  bus.data1_io_num = cfg.d1;
  bus.data2_io_num = cfg.d2;
  bus.data3_io_num = cfg.d3;
  bus.data4_io_num = bus.data5_io_num = bus.data6_io_num = bus.data7_io_num = -1;
  bus.max_transfer_sz = cfg.width * max_rows * 2 + 16;
  bus.flags = SPICOMMON_BUSFLAG_QUAD;
  if (!ok(spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO), "spi_bus_initialize")) return false;

  esp_lcd_panel_io_spi_config_t io_cfg = {};
  io_cfg.cs_gpio_num = static_cast<gpio_num_t>(cfg.cs);
  io_cfg.dc_gpio_num = GPIO_NUM_NC;
  io_cfg.spi_mode = 0;
  io_cfg.pclk_hz = static_cast<uint32_t>(cfg.qspi_mhz) * 1000 * 1000;
  io_cfg.trans_queue_depth = 10;
  io_cfg.lcd_cmd_bits = 32;
  io_cfg.lcd_param_bits = 8;
  io_cfg.flags.quad_mode = 1;
  io_cfg.on_color_trans_done = &Panel::on_trans_done;
  io_cfg.user_ctx = this;
  if (!ok(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kHost), &io_cfg, &io_), "esp_lcd_new_panel_io_spi"))
    return false;

  if (cfg.rst >= 0) {
    gpio_config_t rst = {};
    rst.pin_bit_mask = 1ULL << cfg.rst;
    rst.mode = GPIO_MODE_OUTPUT;
    gpio_config(&rst);
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(static_cast<gpio_num_t>(cfg.rst), 1);
    vTaskDelay(pdMS_TO_TICKS(150));
  }
  for (const auto& c : kInit) {
    command(c.cmd, c.data, c.len);
    if (c.delay_ms) vTaskDelay(pdMS_TO_TICKS(c.delay_ms));
  }
  clear_ram();
  ESP_LOGI(TAG, "CO5300 %ux%u ready, %d MHz QSPI", cfg.width, cfg.height, cfg.qspi_mhz);
  return true;
}

bool Panel::write_rows(int x0, int y0, int width, int rows, const uint16_t* px, void (*done)(void*), void* ctx) {
  rows = std::min(rows, static_cast<int>(cfg_.height) - y0);
  if (rows <= 0) return false;  // the caller completes the band itself
  IoLock l(io_lock_);
  set_window(cfg_.gap_x + x0, cfg_.gap_y + y0, cfg_.gap_x + x0 + width - 1, cfg_.gap_y + y0 + rows - 1);
  // Queued before the transfer (its interrupt can come at once), taken back if refused.
  pending_[pending_tail_ % kPending] = {done, ctx};
  pending_tail_ = pending_tail_ + 1;
  if (esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), px, static_cast<size_t>(rows) * width * 2) == ESP_OK)
    return true;
  pending_tail_ = pending_tail_ - 1;
  return false;
}

void Panel::clear_glass(int x, int y, int width, int rows) {
  if (width <= 0 || rows <= 0) return;
  IoLock l(io_lock_);
  const int chunk = std::min(rows, std::max(8, (4096 / (width * 2)) & ~1));  // ~4 KB, even rows
  const size_t bytes = static_cast<size_t>(width) * chunk * 2;
  void* zero = heap_caps_calloc(1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!zero) ESP_LOGW(TAG, "glass clear: no memory for its buffer; left as it was");
  for (int r = 0; zero && r < rows; r += chunk) {
    const int n = std::min(chunk, rows - r);
    set_window(cfg_.gap_x + x, cfg_.gap_y + y + r, cfg_.gap_x + x + width - 1, cfg_.gap_y + y + r + n - 1);
    if (esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), zero, static_cast<size_t>(n) * width * 2) != ESP_OK ||
        xSemaphoreTake(done_, pdMS_TO_TICKS(100)) != pdTRUE) {
      ESP_LOGW(TAG, "glass clear stopped at row %d", y + r);
      break;
    }
  }
  heap_caps_free(zero);
}

// The controller's memory is wider than the glass. Nothing else ever writes its edges, and after a
// power loss they hold noise (a coloured bar at the rim), so all of it is zeroed once, ~20 ms.
void Panel::clear_ram() {
  IoLock l(io_lock_);
  const int w = cfg_.width + 2 * cfg_.gap_x, h = cfg_.height + 2 * cfg_.gap_y;
  constexpr int kRows = 8;
  void* zero = heap_caps_calloc(1, static_cast<size_t>(w) * kRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (!zero) {
    ESP_LOGW(TAG, "panel memory clear: no memory for its buffer; skipped");
    return;
  }
  for (int y = 0; y < h; y += kRows) {
    const int rows = std::min(kRows, h - y);
    set_window(0, y, w - 1, y + rows - 1);
    if (esp_lcd_panel_io_tx_color(io_, static_cast<int>(kRamWrite), zero, static_cast<size_t>(rows) * w * 2) != ESP_OK ||
        xSemaphoreTake(done_, pdMS_TO_TICKS(100)) != pdTRUE) {
      ESP_LOGW(TAG, "panel memory clear stopped at row %d", y);
      break;
    }
  }
  heap_caps_free(zero);
}

void Panel::set_brightness(uint8_t percent) {
  const uint8_t level = static_cast<uint8_t>(255u * std::min<uint8_t>(percent, 100) / 100u);
  // Zero is display off (the panel keeps its memory: no redraw on waking).
  if (level && !on_) command(0x29, nullptr, 0);
  command(0x51, &level, 1);
  if (!level && on_) command(0x28, nullptr, 0);
  if (!level && deep_sleep_ && !asleep_) {
    command(0x10, nullptr, 0);  // sleep in: the panel's oscillator and converters stop (~30 mA less)
    asleep_ = true;
    slept_us_ = esp_timer_get_time();
  }
  on_ = level != 0;
}

// The CO5300's rules (datasheet, SLPOUT 11h): 5 ms after sleep out before the next command, and
// 120 ms after a sleep in before a sleep out.
void Panel::wake() {
  if (!asleep_) return;
  const int64_t since = esp_timer_get_time() - slept_us_;
  if (since < 120000) vTaskDelay(pdMS_TO_TICKS((120000 - since) / 1000 + 1));
  command(0x11, nullptr, 0);
  vTaskDelay(pdMS_TO_TICKS(6));
  asleep_ = false;
}

}  // namespace board
