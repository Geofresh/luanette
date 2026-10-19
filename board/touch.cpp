// The FT3168 touch controller (the FT5x06 family's report format) through esp_lcd_touch, read on
// its INT line: an idle panel costs no I2C at all.
#include "board.hpp"

#include "esp_lcd_touch_ft5x06.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/task.h"
#include "hal/gpio_ll.h"

namespace board {
namespace {
const char* TAG = "board.touch";
constexpr uint32_t kLiftTimeoutMs = 40;  // a finger down and no report this long: read anyway (a lost lift)
}  // namespace

bool Touch::begin(const TouchConfig& cfg, i2c_master_bus_handle_t bus) {
  if (!bus) return false;
  cfg_ = cfg;
  bus_ = bus;
  esp_lcd_panel_io_i2c_config_t io_cfg = {};
  io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS;
  io_cfg.scl_speed_hz = 400000;
  io_cfg.control_phase_bytes = 1;
  io_cfg.lcd_cmd_bits = 8;
  io_cfg.flags.disable_control_phase = 1;
  esp_lcd_panel_io_handle_t io = nullptr;
  esp_lcd_touch_config_t tc = {};
  tc.x_max = cfg.width;
  tc.y_max = cfg.height;
  tc.rst_gpio_num = cfg.rst >= 0 ? static_cast<gpio_num_t>(cfg.rst) : GPIO_NUM_NC;
  tc.int_gpio_num = GPIO_NUM_NC;  // ours: below
  if (esp_lcd_new_panel_io_i2c(bus, &io_cfg, &io) != ESP_OK) return false;
  if (esp_lcd_touch_new_i2c_ft5x06(io, &tc, &touch_) != ESP_OK) {
    esp_lcd_panel_io_del(io);
    ESP_LOGE(TAG, "no FT3168 on the I2C bus");
    return false;
  }
  gpio_config_t pin = {};
  pin.pin_bit_mask = 1ULL << cfg.int_gpio;
  pin.mode = GPIO_MODE_INPUT;
  pin.pull_up_en = GPIO_PULLUP_ENABLE;
  pin.intr_type = GPIO_INTR_NEGEDGE;
  int_sem_ = xSemaphoreCreateBinary();
  if (!int_sem_ || gpio_config(&pin) != ESP_OK || !gpio_isr_service() ||
      gpio_isr_handler_add(static_cast<gpio_num_t>(cfg.int_gpio), &Touch::on_int, this) != ESP_OK) {
    ESP_LOGE(TAG, "touch INT on GPIO %d unavailable", cfg.int_gpio);
    return false;
  }
  // Core 0: core 1 runs the carts' frames.
  if (xTaskCreatePinnedToCore(&Touch::task, "board-touch", 3072, this, 5, nullptr, 0) != pdPASS) return false;
  ESP_LOGI(TAG, "FT3168 ready, INT on GPIO %d", cfg.int_gpio);
  return true;
}

void IRAM_ATTR Touch::on_int(void* arg) {
  auto* self = static_cast<Touch*>(arg);
  // Armed for light sleep the line is level-triggered: off until the task re-arms it.
  if (self->wake_armed_) gpio_ll_intr_disable(&GPIO, static_cast<uint32_t>(self->cfg_.int_gpio));
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(self->int_sem_, &woken);
  if (woken) portYIELD_FROM_ISR();
}

void Touch::set_wake(bool on) {
  wake_wanted_ = on;
  if (int_sem_) xSemaphoreGive(int_sem_);  // the task applies it now
}

// Armed: INT (high while untouched) wakes on its low level, its interrupt a level one that turns
// itself off; re-armed once the finger is gone. Disarmed: back to the falling edge.
void Touch::apply_wake(bool touching) {
  const auto pin = static_cast<gpio_num_t>(cfg_.int_gpio);
  if (wake_wanted_.load()) {
    if (touching || gpio_get_level(pin) == 0) return;  // wait for the line to rest
    if (!wake_armed_) {
      gpio_wakeup_enable(pin, GPIO_INTR_LOW_LEVEL);
      esp_sleep_enable_gpio_wakeup();
      wake_armed_ = true;
    }
    gpio_intr_enable(pin);
  } else if (wake_armed_) {
    wake_armed_ = false;
    gpio_wakeup_disable(pin);
    gpio_set_intr_type(pin, GPIO_INTR_NEGEDGE);
    gpio_intr_enable(pin);
  }
}

// The FT3168 drops into monitor mode after ~10 idle seconds and doesn't answer until a touch wakes
// it. Monitor mode means nobody is touching: an unanswered probe is "no touch".
bool Touch::chip_answers() const { return i2c_master_probe(bus_, ESP_LCD_TOUCH_IO_I2C_FT5x06_ADDRESS, 20) == ESP_OK; }

void Touch::task(void* arg) {
  auto* self = static_cast<Touch*>(arg);
  bool was_touching = false;
  for (;;) {
    if (was_touching || self->chip_answers()) {
      uint16_t x = 0, y = 0;
      uint8_t points = 0;
      if (esp_lcd_touch_read_data(self->touch_) == ESP_OK) {
        const bool down = esp_lcd_touch_get_coordinates(self->touch_, &x, &y, nullptr, &points, 1) && points > 0;
        // Every sample while the finger is down, and the lift.
        if ((down || was_touching) && self->sink) self->sink({down, static_cast<int16_t>(x), static_cast<int16_t>(y)});
        was_touching = down;
      }
    }
    if (self->wake_wanted_.load() || self->wake_armed_) self->apply_wake(was_touching);
    // A finger down: each report as it lands (the timeout backs up a lost lift). None: the next INT.
    xSemaphoreTake(self->int_sem_, was_touching ? pdMS_TO_TICKS(kLiftTimeoutMs) : portMAX_DELAY);
  }
}

}  // namespace board
