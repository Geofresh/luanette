// Battery percent from a measured discharge curve, apart from the PMIC driver so
// it can be tested on the host. The AXP2101's fuel gauge reads low on some cells
// (the AMOLED-1.75C's 500 mAh cell: 18% at 31% left), and a straight voltage line
// is worse; a curve measured on the board's own cell is close.
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace hg {

struct CurvePoint {
  uint16_t mv;
  uint8_t percent;
};

// Points ordered from full to empty (falling voltage and percent). Linear between
// points; above the first point 100, below the last 0.
struct Curve {
  const CurvePoint* points = nullptr;
  size_t size = 0;
  // Readings (every 5 s) whose median is put on it: BatteryPercent's window, up to 60. Longer for a
  // cell whose voltage dips more under passing loads (a lit screen, the radio).
  size_t window = 12;
  explicit operator bool() const { return points && size >= 2; }
};

inline uint8_t percent_on_curve(Curve curve, uint16_t mv) {
  const CurvePoint* c = curve.points;
  const size_t n = curve.size;
  if (mv >= c[0].mv) return c[0].percent;
  if (mv <= c[n - 1].mv) return c[n - 1].percent;
  for (size_t i = 1; i < n; ++i) {
    const CurvePoint hi = c[i - 1], lo = c[i];
    if (mv >= lo.mv) {
      const uint32_t span = hi.mv - lo.mv, into = mv - lo.mv;
      return static_cast<uint8_t>(lo.percent + ((hi.percent - lo.percent) * into + span / 2) / span);
    }
  }
  return c[n - 1].percent;
}

// The AMOLED-1.75C's 500 mAh cell: one full discharge at about 93 mA (screen
// off, Wi-Fi on), percent = share of the time left until shutdown. 80-95% had no
// readings (the device was offline) and come from a quadratic fit to the rest.
inline constexpr CurvePoint kAmoled175cPoints[] = {
    {4100, 100}, {4090, 99}, {4056, 95}, {4015, 90}, {3974, 85}, {3934, 80}, {3873, 75}, {3813, 63},
    {3754, 55},  {3704, 49}, {3658, 43}, {3624, 37}, {3594, 31}, {3571, 27}, {3530, 21}, {3508, 17},
    {3480, 13},  {3441, 9},  {3408, 7},  {3368, 5},  {3294, 3},  {3157, 1},  {3050, 0},
};
inline constexpr Curve kAmoled175cCurve{kAmoled175cPoints, sizeof(kAmoled175cPoints) / sizeof(kAmoled175cPoints[0])};

// The same cell charging from empty at the PMIC's constant current (200 mA on
// the measured board: the datasheet's charger section gives 300 mA as the default,
// but its register table says ICC's reset value is set in eFuse): percent =
// share of the time to "charge done" (the current only tapered in the last 7 of
// 152 minutes, so charge grew with time). Tops out at 99 until the PMIC reports
// charge done. Checked against a second charge it wasn't fitted to: within 1 point
// on average.
inline constexpr CurvePoint kAmoled175cChargePoints[] = {
    {4200, 99}, {4192, 98}, {4173, 94}, {4161, 90}, {4140, 86}, {4111, 82}, {4076, 78}, {4044, 74},
    {4016, 70}, {3991, 66}, {3966, 62}, {3937, 58}, {3904, 54}, {3868, 50}, {3835, 46}, {3804, 42},
    {3778, 38}, {3754, 34}, {3733, 30}, {3715, 26}, {3698, 22}, {3665, 18}, {3641, 14}, {3613, 10}, {3581, 6},
    {3466, 2}, {3316, 0},
};
inline constexpr Curve kAmoled175cChargeCurve{kAmoled175cChargePoints,
                                              sizeof(kAmoled175cChargePoints) / sizeof(kAmoled175cChargePoints[0])};

// The AMOLED-2.06's 400 mAh cell ("40728"), charged to 4.1 V (battery care), down to the app's
// 3.4 V cut-off (0%): percent = share of the charge left. The shape is a quiet run (2026-10-16:
// 23 h, the screen dark but 35 min, the load near constant, so charge used grew with time) to
// 3.53 V; below, the first run-down's last stretch (2026-10-15, to the cut-off), 18% of the charge.
// (Fractions of the charge, not mAh: there's no current to measure. The charge after it, 2026-10-17,
// put in ~180 mAh at the programmed 200 mA from ~12%, so the cell may hold nearer 200 mAh than
// the label's 400 between 4.1 and 3.4 V.)
// Readings with the screen dark (lit ones sag ~35 mV: Axp2101::set_sag_mv). Medians of 12.
inline constexpr CurvePoint kAmoled206Points[] = {
    {4042, 100}, {3869, 95}, {3826, 90}, {3804, 85}, {3782, 80}, {3756, 75}, {3734, 70}, {3709, 65},
    {3665, 60},  {3653, 55}, {3646, 50}, {3620, 45}, {3608, 40}, {3590, 35}, {3570, 30}, {3556, 25},
    {3536, 20},  {3506, 15}, {3464, 5},  {3400, 0},
};
// 5 minutes of readings: a lit screen sags it 25-110 mV, so a session shorter than half that never
// moves the percent (the never-rising rule would keep what it took); a longer one counts, with the
// sag added back.
inline constexpr Curve kAmoled206Curve{kAmoled206Points, sizeof(kAmoled206Points) / sizeof(kAmoled206Points[0]), 60};

// The same cell charging (2026-10-17 01:55-02:57, from ~12%, the board's 200 mA to 4.1 V, then
// 16 min tapering to the 50 mA termination): percent = share of the charge put in (the constant
// current's time, and the taper as an exponential from 200 to 50 mA). Voltages while charging
// (above the resting ones). In the taper the voltage stays at 4.10 V, so it holds 88-96 until the
// charger says done (100).
inline constexpr CurvePoint kAmoled206ChargePoints[] = {
    {4104, 96}, {4100, 88}, {4067, 82}, {4026, 78}, {3999, 74}, {3965, 70}, {3931, 66}, {3909, 62}, {3881, 58},
    {3866, 54}, {3852, 50}, {3841, 46}, {3834, 42}, {3822, 38}, {3812, 34}, {3807, 30}, {3797, 26}, {3787, 22},
    {3766, 18}, {3734, 14}, {3680, 10},
};
inline constexpr Curve kAmoled206ChargeCurve{kAmoled206ChargePoints, sizeof(kAmoled206ChargePoints) / sizeof(kAmoled206ChargePoints[0])};

// What the device shows, from readings every few seconds, with the median of the
// last discharge.window voltages (12: about a minute) on the curve for the direction of flow:
// - on battery, the discharge curve, never rising: a lighter load lifts the
//   voltage, not the charge;
// - charging, the charge curve (or, without one, the PMIC's gauge), never falling
//   and at most 99 (the curve's top); it starts no lower than what battery showed before plugging in,
//   while the voltage is still climbing; unplugged, battery starts no higher than charging showed;
// - charge done, 100, and still 100 while the PMIC tops the cell up again on USB.
class BatteryPercent {
 public:
  static constexpr size_t kWindow = 12, kMaxWindow = 60;
  explicit BatteryPercent(Curve discharge, Curve charge = {})
      : discharge_(discharge), charge_(charge), n_(std::clamp<size_t>(discharge.window, 1, kMaxWindow)) {}

  enum class Charge { Discharging, Charging, Done };

  uint8_t update(uint16_t mv, Charge charge, std::optional<uint8_t> gauge) {
    if (charge == Charge::Done) {
      flow_ = Flow::Full;
      shown_ = 100;
      return 100;
    }
    const Flow flow = charge == Charge::Charging ? Flow::In : Flow::Out;
    if (flow == Flow::In && !charge_) {
      flow_ = Flow::None;  // no charge curve: the gauge, then a fresh window on battery
      shown_.reset();
      return gauge ? *gauge : percent_on_curve(discharge_, mv);
    }
    if (flow != flow_) {
      // Plugging in keeps what battery showed as a floor, and a top-up after
      // charge done keeps 100. Unplugging keeps what charging showed as a ceiling: the
      // voltage just off the charger reads high (2026-10-18: 15 s on USB at 18% came back
      // as 39%, and took 1.5 h to fall back), and the new window starts from one reading.
      // Only after charge done does it start afresh.
      const bool floor = flow == Flow::In && (flow_ == Flow::Out || flow_ == Flow::Full);
      const bool ceiling = flow == Flow::Out && flow_ == Flow::In;
      if (!floor && !ceiling) shown_.reset();
      flow_ = flow;
      count_ = 0;
    }
    window_[next_] = mv;
    next_ = (next_ + 1) % n_;
    if (count_ < n_) ++count_;
    std::array<uint16_t, kMaxWindow> sorted{};
    for (size_t i = 0; i < count_; ++i) sorted[i] = window_[(next_ + n_ - count_ + i) % n_];
    std::sort(sorted.begin(), sorted.begin() + count_);
    const uint16_t median = sorted[count_ / 2];
    if (flow == Flow::In) {
      const uint8_t now = percent_on_curve(charge_, median);  // charge curves top out at 99
      shown_ = shown_ ? std::max(*shown_, now) : now;
    } else {
      const uint8_t now = percent_on_curve(discharge_, median);
      shown_ = shown_ ? std::min(*shown_, now) : now;
    }
    return *shown_;
  }

 private:
  enum class Flow { None, Out, In, Full };
  Curve discharge_, charge_;
  size_t n_;
  std::array<uint16_t, kMaxWindow> window_{};
  size_t next_ = 0, count_ = 0;
  Flow flow_ = Flow::None;
  std::optional<uint8_t> shown_;
};

}  // namespace hg
