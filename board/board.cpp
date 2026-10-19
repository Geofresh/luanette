// The small drivers: the I2C bus, the GPIO interrupt service, the QMI8658 accelerometer, the
// PCF85063 RTC and the wall clock, and the AXP2101.
#include "board.hpp"

#include <cstdlib>
#include <mutex>
#include <string>
#include <sys/time.h>

#include "esp_log.h"
#include "nvs.h"
#include "pcf85063.hpp"

namespace board {
namespace {
const char* TAG = "board";
}  // namespace

i2c_master_bus_handle_t i2c_bus() {
  static i2c_master_bus_handle_t handle = nullptr;
  if (handle) return handle;
  i2c_master_bus_config_t bus_cfg = {};
  bus_cfg.i2c_port = I2C_NUM_0;
  bus_cfg.sda_io_num = static_cast<gpio_num_t>(kConfig.i2c_sda);
  bus_cfg.scl_io_num = static_cast<gpio_num_t>(kConfig.i2c_scl);
  bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
  bus_cfg.glitch_ignore_cnt = 7;
  bus_cfg.flags.enable_internal_pullup = true;
  if (i2c_new_master_bus(&bus_cfg, &handle) != ESP_OK) {
    ESP_LOGE(TAG, "I2C bus on SDA %d / SCL %d failed", kConfig.i2c_sda, kConfig.i2c_scl);
    handle = nullptr;
  }
  return handle;
}

bool gpio_isr_service() {
  static std::once_flag once;
  static esp_err_t err = ESP_FAIL;
  std::call_once(once, [] { err = gpio_install_isr_service(0); });
  return err == ESP_OK || err == ESP_ERR_INVALID_STATE;  // (installed by someone else: there to use)
}

// ---- the QMI8658 ---------------------------------------------------------------
// Registers from the QMI8658A datasheet: WHO_AM_I 0x00 reads 0x05; CTRL1 0x02 (bit 6: address
// auto-increment), CTRL2 0x03 (accelerometer scale and rate), CTRL7 0x08 (bit 0: accelerometer
// on); AX_L..AZ_H at 0x35..0x3A, little-endian. The first sample comes some tens of ms after it's
// switched on (until then it reads -32768).

bool Imu::write(uint8_t reg, uint8_t value) {
  const uint8_t data[] = {reg, value};
  return i2c_master_transmit(dev_, data, sizeof data, 50) == ESP_OK;
}

bool Imu::begin(i2c_master_bus_handle_t bus, uint8_t addr) {
  if (!bus || !addr) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;
  uint8_t reg = 0x00, who = 0;
  if (i2c_master_transmit_receive(dev_, &reg, 1, &who, 1, 50) != ESP_OK || who != 0x05) {
    ESP_LOGW(TAG, "no QMI8658 at 0x%02x (WHO_AM_I %02x)", addr, who);
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
    return false;
  }
  // Auto-increment; +-2 g at 62.5 Hz (a frame at a time, little power); off until a cart wants it.
  if (!write(0x02, 0x40) || !write(0x03, 0x07) || !write(0x08, 0x00)) {
    ESP_LOGW(TAG, "QMI8658 at 0x%02x didn't take its setup", addr);
    return false;
  }
  return true;
}

bool Imu::enable(bool on) { return dev_ && write(0x08, on ? 0x01 : 0x00); }

bool Imu::read(float* ax, float* ay, float* az) {
  if (!dev_) return false;
  uint8_t reg = 0x35, d[6];
  if (i2c_master_transmit_receive(dev_, &reg, 1, d, sizeof d, 20) != ESP_OK) return false;
  auto raw = [&](int i) { return static_cast<int16_t>(d[i] | d[i + 1] << 8); };
  if (raw(0) == INT16_MIN && raw(2) == INT16_MIN && raw(4) == INT16_MIN) return false;  // no sample yet
  *ax = raw(0) / 16384.0f;
  *ay = raw(2) / 16384.0f;
  *az = raw(4) / 16384.0f;
  return true;
}

// ---- the PCF85063 and the clock ------------------------------------------------

bool Rtc::begin(i2c_master_bus_handle_t bus, uint8_t addr) {
  if (!bus || !addr) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = addr;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;
  uint8_t reg = 0x00, ctrl1 = 0;
  if (i2c_master_transmit_receive(dev_, &reg, 1, &ctrl1, 1, 50) != ESP_OK) {
    ESP_LOGW(TAG, "no PCF85063 at 0x%02x", addr);
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
    return false;
  }
  if (ctrl1 & 0x22) {  // STOP (bit 5) or 12-hour mode (bit 1): run, 24-hour
    const uint8_t w[] = {0x00, static_cast<uint8_t>(ctrl1 & ~0x22)};
    i2c_master_transmit(dev_, w, sizeof w, 50);
  }
  return true;
}

bool Rtc::read(std::time_t* utc) {
  if (!dev_) return false;
  uint8_t reg = hg::pcf85063::kSeconds, r[hg::pcf85063::kTimeRegs];
  if (i2c_master_transmit_receive(dev_, &reg, 1, r, sizeof r, 50) != ESP_OK) return false;
  return hg::pcf85063::decode(r, utc);
}

bool Rtc::write(std::time_t utc) {
  if (!dev_) return false;
  uint8_t w[1 + hg::pcf85063::kTimeRegs] = {hg::pcf85063::kSeconds};
  if (!hg::pcf85063::encode(utc, w + 1)) return false;
  return i2c_master_transmit(dev_, w, sizeof w, 50) == ESP_OK;
}

namespace clock {
namespace {
Rtc* g_rtc = nullptr;
constexpr const char* kNvsSpace = "board_clock";

std::string load_tz() {
  nvs_handle_t h;
  std::string tz;
  if (nvs_open(kNvsSpace, NVS_READONLY, &h) != ESP_OK) return tz;
  size_t len = 0;
  if (nvs_get_str(h, "tz", nullptr, &len) == ESP_OK && len > 1) {
    tz.resize(len);
    if (nvs_get_str(h, "tz", tz.data(), &len) == ESP_OK) tz.resize(len - 1);
    else tz.clear();
  }
  nvs_close(h);
  return tz;
}
}  // namespace

void begin(Rtc* rtc) {
  g_rtc = rtc;
  const std::string tz = load_tz();
  setenv("TZ", tz.empty() ? "UTC0" : tz.c_str(), 1);
  tzset();
  std::time_t t = 0;
  if (rtc && rtc->read(&t)) {
    const timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "the time from the RTC (timezone %s)", tz.empty() ? "UTC" : tz.c_str());
  } else {
    ESP_LOGW(TAG, "the RTC doesn't know the time (set it over USB: push.py --usb sets it with every cart)");
  }
}

bool set(std::time_t utc) {
  const timeval tv = {utc, 0};
  settimeofday(&tv, nullptr);
  return !g_rtc || g_rtc->write(utc);
}

void set_tz(const char* posix) {
  setenv("TZ", posix, 1);
  tzset();
  nvs_handle_t h;
  if (nvs_open(kNvsSpace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "tz", posix);
  nvs_commit(h);
  nvs_close(h);
}
}  // namespace clock

// ---- the AXP2101 ---------------------------------------------------------------

bool Power::begin(i2c_master_bus_handle_t bus) {
  if (!bus || i2c_master_probe(bus, 0x34, 50) != ESP_OK) return false;
  i2c_device_config_t cfg = {};
  cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  cfg.device_address = 0x34;
  cfg.scl_speed_hz = 400000;
  if (i2c_master_bus_add_device(bus, &cfg, &dev_) != ESP_OK) return false;
  chip_ = std::make_unique<hg::Axp2101>(
      [this](uint8_t reg, uint8_t* data, size_t size) {
        return i2c_master_transmit_receive(dev_, &reg, 1, data, size, 50) == ESP_OK;
      },
      [this](uint8_t reg, uint8_t value) {
        const uint8_t data[] = {reg, value};
        return i2c_master_transmit(dev_, data, sizeof(data), 50) == ESP_OK;
      });
  if (!chip_->read()) {
    chip_.reset();
    i2c_master_bus_rm_device(dev_);
    dev_ = nullptr;
    return false;
  }
  return true;
}

}  // namespace board
