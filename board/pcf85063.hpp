#pragma once

// The PCF85063's time registers (NXP PCF85063A datasheet, section 7.4): seconds at 0x04 through
// years at 0x0A, BCD, 24-hour mode. Bit 7 of the seconds register is OS: the oscillator stopped
// (its supply was lost) and the time is not to be trusted until it's set again. The chip keeps
// UTC here (the timezone is the firmware's business), years 2000-2099.
//
// Portable (no ESP-IDF): the conversions are host-tested.

#include <cstdint>
#include <ctime>

namespace hg::pcf85063 {

constexpr uint8_t kSeconds = 0x04;  // the first of the seven time registers
constexpr int kTimeRegs = 7;
constexpr std::time_t kEarliest = 946684800;   // 2000-01-01 00:00 UTC: the chip's year 00
constexpr std::time_t kLatest = 4102444799;    // 2099-12-31 23:59:59 UTC: its year 99

inline uint8_t to_bcd(int v) { return static_cast<uint8_t>((v / 10) << 4 | (v % 10)); }
inline int from_bcd(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

// Days since 1970-01-01 for a civil date (Howard Hinnant's days_from_civil), so no timezone
// (and no timegm, which newlib lacks) is involved.
inline int64_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// The registers' time, as seconds since 1970 (UTC). False when the oscillator stopped or a field
// is out of range (a chip that was never set).
inline bool decode(const uint8_t r[kTimeRegs], std::time_t* out) {
  if (r[0] & 0x80) return false;  // OS: the time was lost
  const int s = from_bcd(r[0] & 0x7F), mi = from_bcd(r[1] & 0x7F), h = from_bcd(r[2] & 0x3F);
  const int d = from_bcd(r[3] & 0x3F), mo = from_bcd(r[5] & 0x1F), y = 2000 + from_bcd(r[6]);
  if (s > 59 || mi > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return false;
  *out = static_cast<std::time_t>(days_from_civil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
  return true;
}

// Seconds since 1970 (UTC) as the registers' values (OS cleared). False outside 2000-2099.
inline bool encode(std::time_t t, uint8_t r[kTimeRegs]) {
  if (t < kEarliest || t > kLatest) return false;
  std::tm tm{};
  gmtime_r(&t, &tm);
  r[0] = to_bcd(tm.tm_sec);
  r[1] = to_bcd(tm.tm_min);
  r[2] = to_bcd(tm.tm_hour);
  r[3] = to_bcd(tm.tm_mday);
  r[4] = static_cast<uint8_t>(tm.tm_wday);
  r[5] = to_bcd(tm.tm_mon + 1);
  r[6] = to_bcd(tm.tm_year - 100);
  return true;
}

}  // namespace hg::pcf85063
