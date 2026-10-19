// The interfaces the carts engine and the board code share: a speaker the sound mixer
// (engine/esp32/sound.hpp) writes PCM to, and the battery's power status (board/axp2101.hpp).
// (From the Hermes Gadget SDK's hg/hal.hpp, MIT.)
#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hg {

class AudioOut {
 public:
  virtual ~AudioOut() = default;
  virtual bool begin(uint32_t sample_rate) = 0;
  virtual void write(const int16_t* samples, size_t count) = 0;
  virtual void end() = 0;
  // Drop anything buffered and stop immediately.
  virtual void abort() = 0;
  virtual bool busy() const = 0;
  virtual void set_volume(uint8_t percent) { (void)percent; }
};

struct PowerStatus {
  std::optional<bool> battery_present;
  std::optional<uint16_t> battery_mv;
  std::optional<uint8_t> battery_percent;
  std::optional<bool> charging;
  std::optional<bool> external_power;
};

class Power {
 public:
  virtual ~Power() = default;
  // A failed read must return nullopt, never the previous reading.
  virtual std::optional<PowerStatus> read() = 0;
  virtual bool can_power_off() const { return true; }
  virtual bool power_off() = 0;
  virtual bool take_key_press() { return false; }
  virtual std::optional<bool> external_power() { return std::nullopt; }
};

}  // namespace hg
