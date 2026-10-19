// The Luanette firmware: the board's drivers up, the console on the panel, then a loop that
// polls the keys and the battery.
#include <cstdio>

#include "board.hpp"
#include "cart_player.hpp"
#include "console.hpp"
#include "esp_log.h"
#include "esp_pm.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "sound.hpp"
#include "usb_link.hpp"

namespace {

const char* TAG = "main";

board::Panel g_panel;
board::Touch g_touch;
board::Buttons g_buttons;
board::Imu g_imu;
board::Rtc g_rtc;
board::Speaker g_speaker;
board::Power g_power;

// The panel starts on core 1, where its transfers' interrupts are then handled (the carts' frames
// run there; the sender, on core 0, isn't interrupted by them).
bool begin_panel_on_core1() {
  struct Job {
    SemaphoreHandle_t done;
    bool ok;
  } job{xSemaphoreCreateBinary(), false};
  if (!job.done) return false;
  auto run = [](void* arg) {
    auto* j = static_cast<Job*>(arg);
    j->ok = g_panel.begin(board::kConfig.panel, carts::player::kBandRows);
    xSemaphoreGive(j->done);
    vTaskDelete(nullptr);
  };
  bool ok = false;
  if (xTaskCreatePinnedToCore(run, "panel-init", 4096, &job, 5, nullptr, 1) == pdPASS) {
    xSemaphoreTake(job.done, portMAX_DELAY);
    ok = job.ok;
  }
  vSemaphoreDelete(job.done);
  return ok;
}

void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

}  // namespace

extern "C" void app_main(void) {
  init_nvs();
  usb_link::begin();
  const auto& cfg = board::kConfig;
  ESP_LOGI(TAG, "Luanette on %s", cfg.name);
  i2c_master_bus_handle_t bus = board::i2c_bus();

  // The battery first: its charger's programme for this cell, and the percent from its own curves.
  const bool power = g_power.begin(bus);
  if (power) {
    g_power.use_curves(cfg.battery_curve, cfg.battery_charge_curve);
    if (!g_power.program_charger(cfg.charge)) ESP_LOGW(TAG, "the charger didn't take its programme");
  } else {
    ESP_LOGW(TAG, "no AXP2101: no battery reading, no power-off");
  }
  board::clock::begin(g_rtc.begin(bus, cfg.rtc_addr) ? &g_rtc : nullptr);

  if (!begin_panel_on_core1()) {
    ESP_LOGE(TAG, "the panel didn't start");
    return;
  }
  const bool imu = g_imu.begin(bus, cfg.imu_addr);
  const bool speaker = g_speaker.begin(cfg.codec, bus);
  if (speaker) carts::sound::begin(&g_speaker, board::Speaker::kRate, nullptr);
  else ESP_LOGW(TAG, "no speaker: carts play silent");
  if (!console::begin({&g_panel, &g_touch, &g_buttons, imu ? &g_imu : nullptr, power ? &g_power : nullptr,
                       speaker ? &g_speaker : nullptr}))
    return;
  g_touch.sink = &console::on_touch;
  if (!g_touch.begin(cfg.touch, bus)) ESP_LOGE(TAG, "no touch");
  g_buttons.begin(cfg.boot_gpio, cfg.pwr_gpio);
  g_buttons.on_boot = [](bool pressed) { console::on_boot(pressed); };
  g_buttons.on_pwr = [] { console::on_pwr(); };

  // Power management: down to 80 MHz when idle, and light sleep while the screen is dark (the
  // console holds both off while it's lit).
  esp_pm_config_t pm = {};
  pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  pm.min_freq_mhz = 80;
  pm.light_sleep_enable = true;
  if (esp_pm_configure(&pm) != ESP_OK) ESP_LOGW(TAG, "no power management: the chip stays at full speed");

  for (;;) {
    g_buttons.poll();
    console::tick_main();
    // Dark and idle: a press (its wake interrupt notifies this task) or a second goes by.
    if (console::sleepy()) ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
    else vTaskDelay(pdMS_TO_TICKS(10));
  }
}
