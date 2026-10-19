// The console: the screen belongs to the carts player (engine/esp32/cart_player.hpp), and when no
// cart is open it runs the launcher, itself a cart (engine/builtin/launcher.lua). It is the
// player's Device on this board, and it owns the screen's power: dim after a minute, dark after
// ten, light sleep while dark (a touch, BOOT or PWR wakes it).
//
// Its own task (core 1) ticks the player under one lock. The USB link and the main loop call in.
#pragma once

#include <string>
#include <string_view>

#include "board.hpp"
#include "hg/json.hpp"

namespace console {

struct Hardware {
  board::Panel* panel;
  board::Touch* touch;      // its samples come in through on_touch
  board::Buttons* buttons;  // polled by the main loop; arm and disarm its wake
  board::Imu* imu;          // or null
  board::Power* power;      // or null
  board::Speaker* speaker;  // or null: the volume setting goes to it
};

// The panel must be started (board::Panel::begin). False: no memory.
bool begin(const Hardware& hw);

void on_touch(const board::TouchSample& s);  // the touch task
void on_boot(bool pressed);                  // BOOT: the carts' B
void on_pwr();                               // PWR: wake; leave the cart; or go dark
void tick_main();                            // the main loop, ~every 10 ms: battery, the low-battery cut-off
bool sleepy();                               // dark and idle: the main loop may wait longer

// A host message (the same ones the USB link carries): carts, their images, the carts list.
bool on_message(const hg::json::Value& msg);
// A line from the USB link; the answer to send back ("@ok ...", "@error ...").
std::string on_line(std::string_view line);

}  // namespace console
