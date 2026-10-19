#include "axp2101.hpp"

namespace hg {

// Battery percent from voltage for when the fuel gauge reports an invalid
// value. This board never initializes the gauge, so it can read 0 on a full
// battery; the voltage (0x34/0x35) is the honest signal. Linear Li-ion map.
static uint8_t millivolts_to_percent(uint16_t mv) {
  constexpr uint16_t kEmptyMv = 3300, kFullMv = 4200;
  if (mv <= kEmptyMv) return 0;
  if (mv >= kFullMv) return 100;
  return static_cast<uint8_t>(static_cast<uint32_t>(mv - kEmptyMv) * 100U / (kFullMv - kEmptyMv));
}

std::optional<PowerStatus> Axp2101::read() {
  uint8_t status[2], enable = 0, adc = 0;
  if (!read_(0x00, status, 2) || !read_(0x18, &enable, 1) || !read_(0x30, &adc, 1)) return std::nullopt;
  PowerStatus out;
  out.battery_present = (status[0] & 0x08) != 0;
  out.external_power = (status[0] & 0x20) != 0;
  out.charging = *out.battery_present && (status[1] & 0x60) == 0x20;
  if (*out.battery_present) {
    if (adc & 0x01) {
      uint8_t voltage[2];
      if (!read_(0x34, voltage, 2)) return std::nullopt;
      const uint16_t mv = static_cast<uint16_t>(((voltage[0] & 0x3f) << 8) | voltage[1]);
      if (mv >= 2000 && mv <= 5000) out.battery_mv = mv;
    }
    std::optional<uint8_t> gauge;
    if (enable & 0x08) {
      // Trust the gauge only in its valid range. A gauge that reads 0 (never
      // initialized on this board) must not read as an empty battery when the
      // voltage says otherwise; derive percent from the voltage instead.
      uint8_t percent = 0;
      if (!read_(0xa4, &percent, 1)) return std::nullopt;  // a failed read is unavailable, not an estimate
      if (percent >= 1 && percent <= 100) gauge = percent;
    }
    if (percent_ && out.battery_mv) {
      // Status 2 (0x01) bits 2:0: 100 charge done; bits 6:5 01 charging.
      using Charge = BatteryPercent::Charge;
      const bool done = *out.external_power && (status[1] & 0x07) == 0x04;
      const Charge charge = done ? Charge::Done : *out.charging ? Charge::Charging : Charge::Discharging;
      const uint16_t sag = charge == Charge::Discharging ? sag_mv_.load(std::memory_order_relaxed) : 0;
      out.battery_percent = percent_->update(static_cast<uint16_t>(*out.battery_mv + sag), charge, gauge);
    } else if (enable & 0x08) {
      if (gauge)
        out.battery_percent = gauge;
      else if (out.battery_mv)
        out.battery_percent = millivolts_to_percent(*out.battery_mv);
    }
  }
  return out;
}

bool Axp2101::power_off() {
  uint8_t config;
  return read_(0x10, &config, 1) && write_(0x10, static_cast<uint8_t>((config & ~0x02) | 0x01));
}

bool Axp2101::enable_aldo1_3v3() {
  uint8_t voltage, enabled;
  if (!read_(0x92, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  // ALDO1: 500 mV + 100 mV per step. Preserve the other rail controls.
  return write_(0x92, static_cast<uint8_t>((voltage & 0xe0) | 28)) &&
         write_(0x90, static_cast<uint8_t>(enabled | 0x01));
}

bool Axp2101::set_aldo3(bool on, int mv) {
  uint8_t voltage, enabled;
  if (!read_(0x94, &voltage, 1) || !read_(0x90, &enabled, 1)) return false;
  // ALDO3 (0x94): 500 mV + 100 mV per step, up to 3.5 V; on/off is bit 2 of 0x90.
  const int step = (mv < 500 ? 500 : mv > 3500 ? 3500 : mv) / 100 - 5;
  return write_(0x94, static_cast<uint8_t>((voltage & 0xe0) | step)) &&
         write_(0x90, static_cast<uint8_t>(on ? enabled | 0x04 : enabled & ~0x04));
}

// IRQ enable 1 (0x41) and IRQ status 1 (0x49), bit 3: PWRON short press.
// Status bits clear when written back as 1.
constexpr uint8_t kKeyShortPress = 0x08;

bool Axp2101::enable_key_press() {
  uint8_t enabled;
  return read_(0x41, &enabled, 1) && write_(0x41, static_cast<uint8_t>(enabled | kKeyShortPress)) &&
         write_(0x49, kKeyShortPress);
}

std::optional<uint16_t> Axp2101::charge_current_ma() {
  uint8_t reg;
  if (!read_(0x62, &reg, 1)) return std::nullopt;
  const unsigned n = reg & 0x1f;  // 25 mA steps to 200 mA, then 100 mA steps
  if (n > 21) return std::nullopt;  // reserved
  return static_cast<uint16_t>(n <= 8 ? 25 * n : 200 + 100 * (n - 8));
}

// 0x61 precharge (25 mA steps), 0x62 ICC, 0x63 termination (bit 4 on, 25 mA
// steps), 0x64 target voltage (1..5 = 4.0/4.1/4.2/4.35/4.4 V), 0x24 VOFF
// (2.6 V + 100 mV steps).
std::optional<Axp2101::ChargeProgramme> Axp2101::charge_programme() {
  uint8_t pre, term, target, voff;
  if (!read_(0x61, &pre, 1) || !read_(0x63, &term, 1) || !read_(0x64, &target, 1) || !read_(0x24, &voff, 1))
    return std::nullopt;
  ChargeProgramme p;
  if ((pre & 0x0f) <= 8) p.precharge_ma = static_cast<uint16_t>(25 * (pre & 0x0f));
  p.cc_ma = charge_current_ma();
  p.term_enabled = (term & 0x10) != 0;
  if ((term & 0x0f) <= 8) p.term_ma = static_cast<uint16_t>(25 * (term & 0x0f));
  static constexpr uint16_t kTargets[] = {0, 4000, 4100, 4200, 4350, 4400};
  if ((target & 0x07) >= 1 && (target & 0x07) <= 5) p.target_mv = kTargets[target & 0x07];
  p.voff_mv = static_cast<uint16_t>(2600 + 100 * (voff & 0x07));
  return p;
}

bool Axp2101::program_charger(const ChargeTarget& t) {
  auto steps25 = [](uint16_t ma) -> int { return ma >= 25 && ma <= 200 && ma % 25 == 0 ? ma / 25 : -1; };
  int cc = steps25(t.cc_ma);
  if (cc < 0 && t.cc_ma >= 300 && t.cc_ma <= 1500 && t.cc_ma % 100 == 0) cc = 8 + (t.cc_ma - 200) / 100;
  int target = -1;
  static constexpr uint16_t kTargets[] = {4000, 4100, 4200, 4350, 4400};
  for (int i = 0; i < 5; ++i)
    if (kTargets[i] == t.target_mv) target = i + 1;
  const int pre = steps25(t.precharge_ma), term = steps25(t.term_ma);
  const int voff = t.voff_mv >= 2600 && t.voff_mv <= 3300 && t.voff_mv % 100 == 0 ? (t.voff_mv - 2600) / 100 : -1;
  if (cc < 0 || target < 0 || pre < 0 || term < 0 || voff < 0) return false;
  struct Field { uint8_t reg, mask, value; };
  const Field fields[] = {
      {0x61, 0x0f, static_cast<uint8_t>(pre)},
      {0x62, 0x1f, static_cast<uint8_t>(cc)},
      {0x63, 0x1f, static_cast<uint8_t>(0x10 | term)},  // termination on
      {0x64, 0x07, static_cast<uint8_t>(target)},
      {0x24, 0x07, static_cast<uint8_t>(voff)},
  };
  for (const Field& f : fields) {
    uint8_t cur;
    if (!read_(f.reg, &cur, 1)) return false;
    const uint8_t want = static_cast<uint8_t>((cur & ~f.mask) | f.value);
    if (want != cur && !write_(f.reg, want)) return false;
  }
  return true;
}

std::optional<bool> Axp2101::vbus_good() {
  uint8_t status;
  if (!read_(0x00, &status, 1)) return std::nullopt;
  return (status & 0x20) != 0;
}

bool Axp2101::take_key_press() {
  uint8_t status;
  if (!read_(0x49, &status, 1) || !(status & kKeyShortPress)) return false;
  write_(0x49, kKeyShortPress);
  return true;
}

}  // namespace hg
