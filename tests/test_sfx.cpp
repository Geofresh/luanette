#include <cstdlib>

#include "check.hpp"
#include "sfx.hpp"

TEST("sfx: every built-in sound renders, the same each time, out of range is silent") {
  for (int n = 0; n < carts::kSfxCount; ++n) {
    const auto a = carts::sfx_render(n, 16000), b = carts::sfx_render(n, 16000);
    CHECK(a.size() > 16000 / 50);   // longer than 20 ms
    CHECK(a.size() < 16000);        // shorter than a second
    CHECK(a == b);
  }
  CHECK(carts::sfx_render(-1, 16000).empty());
  CHECK(carts::sfx_render(carts::kSfxCount, 16000).empty());
}

TEST("sfx: the sounds have room to mix and start and end at silence (no clicks)") {
  for (int n = 0; n < carts::kSfxCount; ++n) {
    const auto pcm = carts::sfx_render(n, 16000);
    int peak = 0;
    for (int16_t s : pcm) peak = std::max(peak, std::abs(static_cast<int>(s)));
    CHECK(peak > 3000);   // audible
    CHECK(peak < 21000);  // a few at once still fit in 16 bits mostly
    CHECK(std::abs(static_cast<int>(pcm.front())) < 300);
    CHECK(std::abs(static_cast<int>(pcm.back())) < 300);
  }
}
