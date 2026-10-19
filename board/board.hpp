// The board: the Waveshare ESP32-S3-Touch-AMOLED-2.06 (a 2.06" AMOLED module with an ESP32-S3). Its pins and settings (kConfig) and
// the drivers Luanette needs: the AMOLED panel (CO5300 over QSPI, written window by
// window), touch (FT3168), BOOT and PWR, the IMU (QMI8658), the RTC (PCF85063), the speaker
// (ES8311 + NS4150B) and the power chip (AXP2101).
//
// Adapted from the Hermes Gadget SDK's ESP32 port (MIT): the same drivers, cut down to this board
// and to what a console needs (no framebuffer mode, no microphones, no Wi-Fi).
//
// Another board: a new kConfig and whichever of these drivers it shares; the console (firmware/
// main/console.cpp) is the carts player's Device and uses only what's declared here.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <optional>

#include "freertos/FreeRTOS.h"  // must precede every other FreeRTOS header

#include "axp2101.hpp"
#include "battery_curve.hpp"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "hg/hal.hpp"
#include "speaker_pa.hpp"

namespace board {

// ---------------------------------------------------------------------------
// Pins and settings

struct PanelConfig {
  uint16_t width, height;              // the glass
  int cs, sclk, d0, d1, d2, d3, rst;   // QSPI and reset
  int gap_x, gap_y;                    // where the glass starts in the controller's memory
  int te;                              // the tearing-effect line: high through each vertical blank
  int qspi_mhz;
  uint16_t corner_radius;              // the glass's rounded corners (carts' caps().corner)
  bool round;
};

struct TouchConfig {
  int rst, int_gpio;
  uint16_t width, height;
};

struct CodecConfig {
  int mclk, bclk, ws, dout, din;
  int pa;  // the amplifier's enable: driven by the speaker, on only while sound plays
};

struct Config {
  const char* name;
  PanelConfig panel;
  int i2c_sda, i2c_scl;
  TouchConfig touch;
  CodecConfig codec;
  int boot_gpio;  // BOOT: active low (the carts' B button)
  int pwr_gpio;   // PWR, mirrored onto a GPIO by a transistor: high while pressed
  uint8_t imu_addr, rtc_addr;
  hg::Axp2101::ChargeTarget charge;  // battery care for the fitted cell, written every boot
  uint16_t cutoff_mv;                // a clean power-off below this, ahead of the brownout
  hg::Curve battery_curve, battery_charge_curve;
  uint16_t lit_sag_mv;  // what a lit screen takes off the battery's voltage (added back for the percent)
};

// The board. Pins from Waveshare's schematic and BSP; the measurements are dated where they were made.
inline constexpr Config kConfig{
    "waveshare-esp32s3-touch-amoled-2.06",
    // 410x502 glass, the controller's memory 22 columns wider on the left; 80 MHz QSPI (full frames
    // ~8 ms on the wire); the corners' radius measured with a grid (2026-10-14).
    {410, 502, 12, 11, 4, 5, 6, 7, 8, 22, 0, 13, 80, 100, false},
    15, 14,
    {9, 38, 410, 502},  // FT3168: reset on GPIO 9, INT on 38 (pulses low on a report)
    {16, 41, 45, 40, 42, 46},
    0, 10,
    0x6B, 0x51,
    // The cell: 3.7 V 400 mAh. Battery care: 0.5 C, a 4.1 V ceiling, termination at C/8, the
    // cut-off raised from the factory 2.6 V to 2.9 V.
    {50, 200, 50, 4100, 2900},
    // Run down (2026-10-15) the panel flickered from ~3.2 V and the board died at ~3.0 V; 3.4 V
    // came ~40 min before the end under heavy use.
    3400,
    hg::kAmoled206Curve, hg::kAmoled206ChargeCurve,
    35,
};

// ---------------------------------------------------------------------------
// Shared helpers

// The I2C bus (created on first use).
i2c_master_bus_handle_t i2c_bus();
// The GPIO interrupt service, shared by every driver that adds a pin handler.
bool gpio_isr_service();

// ---------------------------------------------------------------------------
// The panel: no framebuffer. Rows go from DMA-capable memory to any window of the glass, and a
// callback runs from the transfer's interrupt when each is done. Windows start and end on even
// columns and rows (the CO5300's rule).
class Panel {
 public:
  bool begin(const PanelConfig& cfg, int max_rows);
  bool write_rows(int x0, int y0, int width, int rows, const uint16_t* px, void (*done)(void*), void* ctx);
  void clear_glass(int x, int y, int width, int rows);  // no write_rows may be in flight
  void set_brightness(uint8_t percent);  // 0: display off (and asleep, with set_deep_sleep)
  void set_deep_sleep(bool on) { deep_sleep_ = on; }
  void wake();  // out of a deep sleep, before the next frame
  const PanelConfig& config() const { return cfg_; }

 private:
  static bool on_trans_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t* edata, void* ctx);
  void command(uint8_t cmd, const uint8_t* data, size_t len);
  void set_window(int x0, int y0, int x1, int y1);
  void clear_ram();
  PanelConfig cfg_{};
  SemaphoreHandle_t io_lock_ = nullptr;  // every panel transaction (more than one task sends)
  SemaphoreHandle_t done_ = nullptr;     // the clears' own transfers
  esp_lcd_panel_io_handle_t io_ = nullptr;
  struct Pending {
    void (*cb)(void*);
    void* ctx;
  };
  static constexpr unsigned kPending = 16;  // more than the SPI queue holds
  Pending pending_[kPending] = {};
  volatile unsigned pending_head_ = 0, pending_tail_ = 0;
  bool deep_sleep_ = false, asleep_ = false, on_ = true;
  int64_t slept_us_ = 0;
};

// ---------------------------------------------------------------------------
// Touch: the FT3168, read on its INT line (a report at ~64 Hz while a finger is down, nothing
// otherwise). Samples go to `sink` from the touch task.
struct TouchSample {
  bool touching;
  int16_t x, y;
};

class Touch {
 public:
  bool begin(const TouchConfig& cfg, i2c_master_bus_handle_t bus);
  void (*sink)(const TouchSample&) = nullptr;  // set before begin()
  // Light sleep: while on, a touch wakes the chip (INT as a low-level wake source).
  void set_wake(bool on);

 private:
  static void task(void* arg);
  static void on_int(void* arg);
  void apply_wake(bool touching);
  bool chip_answers() const;
  TouchConfig cfg_{};
  esp_lcd_touch_handle_t touch_ = nullptr;
  i2c_master_bus_handle_t bus_ = nullptr;
  SemaphoreHandle_t int_sem_ = nullptr;
  std::atomic<bool> wake_wanted_{false};
  volatile bool wake_armed_ = false;
};

// ---------------------------------------------------------------------------
// BOOT and PWR, polled every ~10 ms with a short debounce. Callbacks run on the polling task.
class Buttons {
 public:
  void begin(int boot_gpio, int pwr_gpio);
  void poll();
  std::function<void(bool pressed)> on_boot;
  std::function<void()> on_pwr;  // a press of PWR
  // Light sleep: while on, a press wakes the chip, and `woken` (the polling task) is notified.
  void set_wake(bool on, TaskHandle_t woken);
  bool idle() const;  // nothing down or settling: the poll may slow down

 private:
  struct Key {
    int gpio = -1;
    bool active_high = false;
    bool pressed = false;
    uint8_t stable = 0;
    bool wake = false;
  };
  static void on_wake_isr(void* arg);
  void apply_wake();
  Key boot_, pwr_;
  std::atomic<bool> wake_wanted_{false};
  bool wake_armed_ = false, isr_ready_ = false;
};

// ---------------------------------------------------------------------------
// The QMI8658's accelerometer: off until enabled; readings in g, the chip's axes.
class Imu {
 public:
  bool begin(i2c_master_bus_handle_t bus, uint8_t addr);
  bool enable(bool on);
  bool read(float* ax, float* ay, float* az);

 private:
  bool write(uint8_t reg, uint8_t value);
  i2c_master_dev_handle_t dev_ = nullptr;
};

// ---------------------------------------------------------------------------
// The PCF85063 RTC (UTC, kept through power-off by the AXP2101's RTC supply), and the wall clock
// it sets at boot. The timezone (a POSIX TZ string) is kept in NVS.
class Rtc {
 public:
  bool begin(i2c_master_bus_handle_t bus, uint8_t addr);
  bool read(std::time_t* utc);  // false: no chip, or it lost its time
  bool write(std::time_t utc);

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
};

namespace clock {
void begin(Rtc* rtc);                 // the system time from the RTC; rtc may be null
bool set(std::time_t utc);            // the system time and the RTC
void set_tz(const char* posix);       // e.g. "EST5EDT,M3.2.0,M11.1.0"; kept in NVS
}  // namespace clock

// ---------------------------------------------------------------------------
// The speaker: the ES8311 on I2S at 16 kHz, its amplifier on only while sound plays. The codec
// powers down after 2 s of silence and comes back for the next sound.
class Speaker final : public hg::AudioOut {
 public:
  static constexpr uint32_t kRate = 16000;
  bool begin(const CodecConfig& cfg, i2c_master_bus_handle_t bus);
  bool begin(uint32_t sample_rate) override;
  void write(const int16_t* samples, size_t count) override;
  void end() override;
  void abort() override;
  bool busy() const override;
  void set_volume(uint8_t percent) override;
  bool asleep() const { return closed_; }

 private:
  static void task(void* arg);
  void set_sleep(bool sleep);
  i2s_chan_handle_t tx_ = nullptr;
  esp_codec_dev_handle_t dev_ = nullptr;
  esp_codec_dev_sample_info_t fs_ = {};
  TaskHandle_t task_ = nullptr;
  std::optional<hg::SpeakerPa> pa_;
  StreamBufferHandle_t buffer_ = nullptr;
  std::atomic<bool> open_{false}, draining_{false}, flush_{false};
  volatile bool closed_ = false;
  uint8_t volume_ = 70;
};

// ---------------------------------------------------------------------------
// The AXP2101: the battery (voltage, a percent from this cell's own curves, charging), its
// charger's programme, and power-off.
class Power final : public hg::Power {
 public:
  bool begin(i2c_master_bus_handle_t bus);
  std::optional<hg::PowerStatus> read() override { return chip_ ? chip_->read() : std::nullopt; }
  bool power_off() override { return chip_ && chip_->power_off(); }
  std::optional<bool> external_power() override { return chip_ ? chip_->vbus_good() : std::nullopt; }
  void use_curves(hg::Curve discharge, hg::Curve charge) {
    if (chip_ && discharge) chip_->use_curves(discharge, charge);
  }
  bool program_charger(const hg::Axp2101::ChargeTarget& t) { return chip_ && chip_->program_charger(t); }
  void set_sag_mv(uint16_t mv) {
    if (chip_) chip_->set_sag_mv(mv);
  }

 private:
  i2c_master_dev_handle_t dev_ = nullptr;
  std::unique_ptr<hg::Axp2101> chip_;
};

}  // namespace board
