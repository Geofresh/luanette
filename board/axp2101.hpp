#pragma once

#include <atomic>
#include <functional>
#include <utility>

#include "battery_curve.hpp"
#include "hg/hal.hpp"

namespace hg {

// Register definitions follow X-Powers AXP2101 SWcharge v1.0, section 6.13.
// The port owns I2C. Reading never changes charging, rails, or gauge parameters.
class Axp2101 final : public Power {
 public:
  using Read = std::function<bool(uint8_t, uint8_t*, size_t)>;
  using Write = std::function<bool(uint8_t, uint8_t)>;
  Axp2101(Read read, Write write) : read_(std::move(read)), write_(std::move(write)) {}
  std::optional<PowerStatus> read() override;
  bool power_off() override;
  bool enable_aldo1_3v3();  // Only for boards whose audio circuit requires this rail.
  bool set_aldo3(bool on, int mv);  // The AMOLED-2.06's vibration motor supply.
  // The power key, for boards where only the PMIC sees it. Enabling changes the
  // interrupt registers only, and drops a press latched before it.
  bool enable_key_press();
  bool take_key_press();  // true once per short press
  std::optional<bool> vbus_good();  // external power present (status 0x00 bit 5)
  bool set_reg(uint8_t address, uint8_t value) { return write_(address, value); }  // bench experiments
  std::optional<uint8_t> reg(uint8_t address) {  // a raw register, for diagnostics
    uint8_t v;
    return read_(address, &v, 1) ? std::optional<uint8_t>(v) : std::nullopt;
  }
  // Curves measured on the board's cell. With them the percent is the smoothed
  // discharge curve on battery, the charge curve (or the gauge) while charging and
  // 100 once charged (BatteryPercent); without, the gauge or the linear fallback.
  void use_curves(Curve discharge, Curve charge = {}) { percent_.emplace(discharge, charge); }
  // A heavier load's voltage drop (the screen lit: ~35 mV on the AMOLED-2.06's cell), added back to
  // the battery's readings for the percent on battery, so a lit screen doesn't take percent that
  // the charge didn't lose (the reported voltage stays as read). 0: none. Any task may set it.
  void set_sag_mv(uint16_t mv) { sag_mv_.store(mv, std::memory_order_relaxed); }
  // The constant-current charge limit (ICC, 0x62), for diagnostics. Read only.
  std::optional<uint16_t> charge_current_ma();
  // The charger's whole programme as the eFuse or a previous boot left it, for
  // diagnostics. Read only; a field the chip reports as reserved stays empty.
  struct ChargeProgramme {
    std::optional<uint16_t> precharge_ma, cc_ma, term_ma, target_mv, voff_mv;
    bool term_enabled = false;
  };
  std::optional<ChargeProgramme> charge_programme();
  // Battery care: writes the charger's programme and the cut-off voltage, each
  // register read-modify-write and only when it differs (the chip keeps them
  // across an ESP32 reset; a full power loss returns the eFuse values, so this
  // runs every boot). Currents: precharge and termination 25..200 mA in 25 mA
  // steps, CC 25..200 in 25 or 300..1500 in 100; target 4000/4100/4200/4350/
  // 4400 mV; VOFF 2600..3300 mV in 100. False on an unencodable value (nothing
  // is written then) or an I2C failure.
  struct ChargeTarget {
    uint16_t precharge_ma, cc_ma, term_ma, target_mv, voff_mv;
  };
  bool program_charger(const ChargeTarget& t);

 private:
  Read read_;
  Write write_;
  std::optional<BatteryPercent> percent_;
  std::atomic<uint16_t> sag_mv_{0};
};

}  // namespace hg
