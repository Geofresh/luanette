// The cart player: runs a cart (cart.hpp) on the glass and sends its frames, as few pixels as
// changed, each band timed against the panel's refresh (scan_sync.hpp) so frames land whole. A 3D
// cart's scene (stage.hpp) is drawn band by band on both cores as it goes out.
//
// One board so far: the Waveshare ESP32-S3-Touch-AMOLED-2.06 (410x502, the picture 3x at 12, 60).
// The firmware hosting it (firmware/main/console.cpp) implements Device; the player
// owns the screen while a cart is open and gives it back on close.
//
//   carts::player::begin(device, library, {idle_prio, frame_prio});
//   carts::player::open("dice");             // on the task that ticks, under the host's lock
//   for (;;) sleep_ms(carts::player::tick());
#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "freertos/FreeRTOS.h"
#include "hg/json.hpp"

namespace carts::store {
class Library;
}

namespace carts::player {

// The band buffers' height: the device's two buffers each hold this many rows of the glass's width.
constexpr int kBandRows = 32;

// The newest touch sample, in glass pixels.
struct Touch {
  bool touching = false;
  int x = 0, y = 0;
  int64_t t_us = 0;          // when it was read (esp_timer)
  int down_x = 0, down_y = 0;  // where this finger came down
};

class Device {
 public:
  virtual ~Device() = default;
  // The glass
  virtual int width() const = 0;
  virtual int height() const = 0;
  virtual int corner_radius() const { return 0; }
  virtual bool round() const { return false; }
  virtual void clear_glass() = 0;
  // Two band buffers (kBandRows rows of the glass's width, DMA-capable), filled and sent alternately.
  virtual uint16_t* band(int k) = 0;
  virtual void band_wait(int k) = 0;  // until band k's last transfer is done
  // Band k's first w * h pixels (rows of w) to the glass at (x, y), asynchronously. False: refused.
  virtual bool band_send(int k, int x, int y, int w, int h) = 0;
  // The screen and input
  virtual bool screen_on() const = 0;
  virtual void wake_screen() = 0;  // a cart's wake(): light it
  virtual Touch touch() = 0;
  virtual void note_activity() {}
  // Motion and vibration, where the board has them
  virtual bool has_imu() const { return false; }
  virtual bool imu_enable(bool on) { (void)on; return false; }
  virtual bool imu_read(float* x, float* y, float* z) { (void)x, (void)y, (void)z; return false; }
  virtual bool has_motor() const { return false; }
  virtual void buzz(int ms) { (void)ms; }
  virtual void motor_rest() {}
  // The host: its data channels (a cart's data()), and messages to it (act(); "cart" with the name
  // of the cart that opened, "" when it closed).
  virtual const hg::json::Value* data(std::string_view channel, uint32_t* version) = 0;
  virtual void command(std::string_view name, std::string_view arg) = 0;
  // The host's screen around a cart: before it opens (stop drawing, let the panel go) and after it
  // closed (draw again). Both on the ticking task.
  virtual void will_open() {}
  virtual void closed() {}
  // Housekeeping
  virtual void timed_out(const char* what) = 0;  // a wait that ran out (logged, counted)
  virtual void frame_out_begin() {}  // around each frame's sending (core 0)
  virtual void frame_out_end() {}
  virtual bool profile_start() { return false; }  // a sampling profile over cart frames, if the host has one
  virtual void profile_stop(const char* what) { (void)what; }
  virtual double lock_wait_ms(bool others) { (void)others; return 0; }  // the host's lock, longest wait since last asked (stats)
};

struct Config {
  UBaseType_t idle_prio = 4;   // the ticking task's priority, normally
  UBaseType_t frame_prio = 6;  // and while a cart runs (its frames on the pass, ahead of the app's tasks)
};

void begin(Device& device, store::Library& library, const Config& config);
// A cart by name (the library's, or "@stage...": the 3D test scene). False: no such cart, or no memory.
bool open(const std::string& name);
void close();
bool running();
const std::string& name();  // the open cart's ("" when none)
// Each loop of the ticking task while a cart runs: returns how long it may sleep (ms). May close
// the cart (a swipe down from the top edge: closed_by_gesture()).
uint32_t tick();
bool closed_by_gesture();  // since last asked: the rest of that swipe belongs to nobody
bool slow();               // a frame every 100 ms or more: the host may nap between them
bool stopped();            // the open cart stopped with an error (its message on the glass)
const std::string& error();  // that error ("" when none)
void relit();              // the screen lit again: everything is sent anew
bool screenshot(uint16_t* px);  // the glass as the cart shows it (width x height, zeroed by the caller); false: busy
void set_button_b(bool down);   // BOOT

// Tuning aids (the console's `check` commands): profile / count / check over the next n frames.
struct Tuning {
  std::atomic<int> profile_frames{0};
  std::atomic<int> profile_core{0};  // a stage's: 1 samples its prepare (core 1), else its drawing
  std::atomic<int> panel_check{0};
  std::atomic<int> perf_frames{0};
  // Chip axes to the screen's (right, down): which chip axis (0-2) and its sign, for each
  // (tilt = sign * reading). The QMI8658 sits turned a quarter on the AMOLED-2.06's board: its +y
  // reads toward the screen's left (a reading of -y: the right edge is lower) and its +x toward the
  // bottom; +z is out of the back (face up flat: z = -1 g). Found from the board held each way
  // (2026-10-14; the first guess, {0, 1, 1, 1}, turned every tilt a quarter).
  int tilt[4] = {1, -1, 0, 1};
};
extern Tuning tuning;

}  // namespace carts::player
