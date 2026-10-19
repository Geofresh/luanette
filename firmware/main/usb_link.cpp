// The USB link: the ESP32-S3's own USB serial port, one line in, one answer out (console::on_line;
// tools/push.py speaks it). The log goes out on the same port, so a host reads the lines that start
// with '@' as answers and passes the rest by. No echo and no line editing: a cart's base64 parts
// arrive at the port's own speed.
#include "usb_link.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "console.hpp"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace usb_link {
namespace {

const char* TAG = "usb";
constexpr size_t kMaxLine = 8192;

// An answer out, straight to the driver in blocks: through stdio the driver takes one character
// at a time, a lock and a ring-buffer call each, and a screenshot's 270 KB kept core 0 (and the
// frame sender on it) busy for seconds. A block waits for room instead of spinning; a host that
// stops reading costs half a second, then the rest is dropped.
void send(const std::string& text) {
  std::fflush(stdout);  // the log so far first
  for (size_t off = 0; off < text.size();) {
    const size_t n = std::min<size_t>(1024, text.size() - off);
    const int sent = usb_serial_jtag_write_bytes(text.data() + off, n, pdMS_TO_TICKS(500));
    if (sent <= 0) {
      ESP_LOGW(TAG, "the host stopped reading: %u of %u bytes of an answer dropped", static_cast<unsigned>(text.size() - off),
               static_cast<unsigned>(text.size()));
      return;
    }
    off += static_cast<size_t>(sent);
  }
}

void task(void*) {
  std::string line;
  uint8_t buf[512];
  for (;;) {
    const int n = usb_serial_jtag_read_bytes(buf, sizeof buf, portMAX_DELAY);
    for (int i = 0; i < n; ++i) {
      const char c = static_cast<char>(buf[i]);
      if (c == '\n') {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) send(console::on_line(line) + "\n");
        line.clear();
      } else if (line.size() < kMaxLine) {
        line.push_back(c);
      }
    }
  }
}

}  // namespace

void begin() {
  // The driver, with room for a few lines in flight; the log goes through it too (the bare port
  // without it reads a 64-byte FIFO, and loses characters whenever the reader is busy).
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  cfg.rx_buffer_size = 16384;
  cfg.tx_buffer_size = 4096;
  if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
    ESP_LOGE(TAG, "no USB serial driver: carts can't arrive over USB");
    return;
  }
  usb_serial_jtag_vfs_use_driver();
  if (xTaskCreate(task, "usb-link", 6144, nullptr, 5, nullptr) != pdPASS) ESP_LOGE(TAG, "no USB link task");
}

}  // namespace usb_link
