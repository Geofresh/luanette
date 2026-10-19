// The carts' sounds on the board's speaker: sfx(n) plays one of the built-in effects
// (sfx.hpp). A small mixer task (up to four at once: a coin over an explosion) writes them to
// the speaker in 10 ms frames while any plays, then lets the speaker go (its amplifier draws power
// only while sound plays). A host that also uses the speaker for something else (a voice, an
// alarm) passes `voice_busy`: while it says so, sounds are dropped.
#pragma once
#include <cstdint>

#include "hg/hal.hpp"

namespace carts::sound {

// The speaker at `rate` Hz; voice_busy() (or null) true while the host has the speaker. Once, at start.
void begin(hg::AudioOut* speaker, uint32_t rate, bool (*voice_busy)());
bool available();  // a speaker to play on
void play(int n);  // any task; returns at once
void set_muted(bool muted);  // muted: play() drops sounds and the speaker stays closed

}  // namespace carts::sound
