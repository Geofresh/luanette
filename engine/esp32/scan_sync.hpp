// The panel's refresh, for sending frames that land whole: a panel with a TE (tearing effect) line
// refreshes itself from its own memory at ~60 Hz, top row to bottom, and pulses TE through each
// vertical blank. Bands written just behind the scan show in full on the next pass (te_gate), or
// just ahead of it on the pass now starting ("racing the beam"). The cart player (cart_player.hpp)
// sends through this. ESP32 (esp_timer, a GPIO interrupt).
#pragma once
#include <atomic>
#include <cstdint>

namespace carts::scan {

// What the timing needs to know about the panel. Set once before te_begin().
struct Config {
  int rows = 1;                         // the scan's rows: the glass's height (plus any offset's room)
  std::atomic<int>* y_offset = nullptr;  // a picture moved down the glass by this many rows (an anti burn-in orbit), or none
  int band_rows = 32;                   // the senders' band height (for the late bands' histogram)
  // The host's way to have the shared GPIO interrupt service installed (other drivers add pin
  // handlers too, and a second install logs an error); null: installed here.
  bool (*gpio_isr_service)() = nullptr;
};
void configure(const Config& c);

// The panel refreshes itself from its own memory at ~60 Hz, top row to bottom, and raises TE
// through each vertical blank: the scan of row 0 starts on the falling edge. A band written
// where the scan has yet to pass shows half old, half new (the seam of a fast scroll), and
// frames finishing at random points of the panel's cycle show for uneven times (judder).
// So each band waits until the scan has passed its rows (it shows in full on the next pass),
// and every frame gets its own pass: a frame's bands all go out behind one pass's scan and
// ahead of the next one's, which holds for frames rendering in up to about two cycles.
struct Te {
  int pin = -1;
  std::atomic<int64_t> fall_us{0}, rise_us{0}, period_us{0}, high_us{0};
  std::atomic<uint32_t> falls{0};
  bool on = true;                // the switch (a host may turn TE gating off to compare)
  bool armed = false;            // its interrupt is enabled (the screen is lit)
  int guard_rows = 8;            // how far behind the scan a band waits (tunable: 4-16)
  int send_permille = 600;       // a band's transfer time against its scan time (tunable: 500-800)
  bool frame_open = false;       // this frame's first band has gone (and fixed its pass)
  int64_t frame_pass = 0, last_pass = INT64_MIN / 2;
  int64_t wait_us = 0;           // for the stats line
  int64_t wait_total_us = 0;     // never reset: frame records take differences
  uint32_t late = 0, bands = 0;  // bands that missed their window / all gated
  // Racing the beam (on unless a host turns it off): bands written just ahead of the
  // scan show on the pass now starting, not the next one: ~a refresh less from touch to photon.
  bool ahead = true;  // on since 2026-10-13: 100% locked, no late bands in the benches (swipe task)
  uint32_t ahead_frames = 0, ahead_late = 0;  // frames raced, and bands that couldn't beat the scan
  int64_t ahead_t0 = 0;                       // the raced frame's start (input latch), for the stats
  int64_t ahead_prep_max = 0;                 // start to band 0 queued, worst in the stats window
  uint32_t ahead_late_band[16] = {};          // which bands were late
  uint32_t frames_at = 0, frames_lvgl = 0, frames_empty = 0;  // direct frames by compose path; empty: nothing composed
  int64_t first_dur = 0, first_wait = 0, first_compose = 0, first_gap = 0;  // a swipe's first raced frame (worst)
  // Racing LVGL frames (the reader scrolling): set around a paced refresh, read by te_gate (the
  // flush worker's bands and the body paint): bands go just ahead of pass race_pass's scan.
  int64_t race_pass = -1;
  bool race_failed = false;  // a band couldn't beat the scan: the rest of the frame the usual way
  uint32_t race_bands = 0;
  uint32_t reader_raced = 0, reader_fellback = 0;  // reader frames raced, and fallen back whole (a pass later)
  int64_t prefetch_max = 0;                         // the worst ring strip drawn ahead in a frame's slack
  bool first_pending = false;
};
extern Te te;

// TE on `pin` (any edge, an interrupt); a pin < 0, or no interrupt: frames go out unsynchronised.
void te_begin(int pin);
// Off with the screen: a display-off panel may keep pulsing, and nothing needs it.
void te_arm(bool on);
// Until time t (esp_timer microseconds), to the microsecond, the core free for most of it.
void wait_until(int64_t t);
// Before a band of rows y0..y1 goes out. `first`: the frame's first band.
void te_gate(int y0, int y1, bool first);

// The scan's clock, as te_gate reads it: pass n starts at fall + (n - base) * period (its row 0),
// and reaches row y `active * y / rows` later.
struct ScanClock {
  int64_t period = 0, fall = 0, active = 0;
  uint32_t base = 0;
  int rows = 1, shift = 0;  // the scan's rows, and the picture's offset down them (set from Config)
  bool ok = false;
  int64_t start(int64_t n) const { return fall + (n - static_cast<int64_t>(base)) * period; }
  int64_t at(int y) const { return static_cast<int64_t>(y + shift) * active / rows; }
};
ScanClock scan_clock();

// Touch to photon (the stats lines): sums over frames drawn with a finger down.
struct Latency {
  int64_t age = 0, work = 0, scan = 0;
  uint32_t n = 0;
};
extern Latency lat;
void lat_account(int64_t age, int64_t start_us, int64_t done_us);

}  // namespace carts::scan
