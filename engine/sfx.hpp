// The carts' built-in sounds (sfx(n), docs/api.md): eight short effects synthesized as 16-bit
// mono PCM, in the spirit of an 8-bit console's sound chip (square, triangle and noise voices with
// pitch slides and envelopes). Portable: the board plays them (esp32/sound.hpp), and the desktop
// build renders them to WAV files to listen to (host/sfx_wav.cpp).
#pragma once
#include <cstdint>
#include <vector>

namespace carts {

constexpr int kSfxCount = 8;  // 0 blip, 1 coin, 2 hit, 3 jump, 4 explosion, 5 select, 6 error, 7 success

// Sound n at `rate` samples a second (empty for an n out of range). Same input, same samples.
std::vector<int16_t> sfx_render(int n, int rate);

}  // namespace carts
