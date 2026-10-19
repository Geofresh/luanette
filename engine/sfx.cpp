#include "sfx.hpp"

#include <algorithm>
#include <cmath>

namespace carts {
namespace {

enum class Wave : uint8_t { Square, Triangle, Noise, Rest };

// One step of a sound: a voice gliding from f0 to f1 Hz (exponentially, as pitch is heard) over
// ms milliseconds, its loudness from v0 to v1 (0-1). A square's duty is its high part (0.5: even).
// A noise's "frequency" is how often it picks a new random level: high hisses, low rumbles.
struct Step {
  Wave wave;
  float f0, f1;
  int ms;
  float v0, v1;
  float duty = 0.5f;
};

struct Sound {
  const Step* steps;
  int n;
};

constexpr Step kBlip[] = {{Wave::Square, 1046, 1046, 45, 0.5f, 0.0f}};
constexpr Step kCoin[] = {{Wave::Square, 988, 988, 60, 0.45f, 0.4f, 0.25f}, {Wave::Square, 1319, 1319, 220, 0.45f, 0.0f, 0.25f}};
constexpr Step kHit[] = {{Wave::Noise, 2200, 800, 70, 0.55f, 0.15f}, {Wave::Square, 160, 60, 90, 0.35f, 0.0f}};
constexpr Step kJump[] = {{Wave::Square, 260, 760, 160, 0.45f, 0.1f}};
constexpr Step kExplosion[] = {{Wave::Noise, 1800, 120, 500, 0.6f, 0.0f}};
constexpr Step kSelect[] = {{Wave::Triangle, 784, 784, 50, 0.6f, 0.5f}, {Wave::Triangle, 1175, 1175, 80, 0.6f, 0.0f}};
constexpr Step kError[] = {{Wave::Square, 233, 233, 120, 0.4f, 0.35f}, {Wave::Rest, 0, 0, 30, 0, 0}, {Wave::Square, 175, 175, 200, 0.4f, 0.0f}};
constexpr Step kSuccess[] = {{Wave::Triangle, 523, 523, 70, 0.55f, 0.45f},
                             {Wave::Triangle, 659, 659, 70, 0.55f, 0.45f},
                             {Wave::Triangle, 784, 784, 70, 0.55f, 0.45f},
                             {Wave::Triangle, 1046, 1046, 220, 0.55f, 0.0f}};

constexpr Sound kSounds[kSfxCount] = {
    {kBlip, 1},      {kCoin, 2},   {kHit, 2},   {kJump, 1},
    {kExplosion, 1}, {kSelect, 2}, {kError, 3}, {kSuccess, 4},
};

constexpr float kPeak = 0.6f * 32767;  // headroom for a few sounds mixed together
constexpr float kAttackS = 0.002f, kFadeS = 0.004f;  // no clicks at a sound's ends

}  // namespace

std::vector<int16_t> sfx_render(int n, int rate) {
  std::vector<int16_t> out;
  if (n < 0 || n >= kSfxCount || rate <= 0) return out;
  const Sound& s = kSounds[n];
  size_t total = 0;
  for (int i = 0; i < s.n; ++i) total += static_cast<size_t>(s.steps[i].ms) * rate / 1000;
  out.reserve(total);
  float phase = 0;      // 0-1, carried from step to step: no click where they meet
  uint32_t lfsr = 0xACE1u;  // the noise voice's generator (a 16-bit Galois LFSR)
  float level = 0;      // the noise's current random level
  for (int i = 0; i < s.n; ++i) {
    const Step& st = s.steps[i];
    const int len = st.ms * rate / 1000;
    for (int k = 0; k < len; ++k) {
      const float t = len > 1 ? static_cast<float>(k) / (len - 1) : 0.0f;
      const float f = st.f0 > 0 ? st.f0 * std::pow(st.f1 / st.f0, t) : 0.0f;
      const float v = st.v0 + (st.v1 - st.v0) * t;
      float x = 0;
      const float before = phase;
      phase += f / rate;
      if (phase >= 1.0f) phase -= std::floor(phase);
      switch (st.wave) {
        case Wave::Square: x = phase < st.duty ? 1.0f : -1.0f; break;
        case Wave::Triangle: x = phase < 0.5f ? 4 * phase - 1 : 3 - 4 * phase; break;
        case Wave::Noise:
          if (phase < before) {  // a new level each cycle
            lfsr = (lfsr >> 1) ^ (-(lfsr & 1u) & 0xB400u);
            level = (lfsr & 0xFFFF) / 32767.5f - 1.0f;
          }
          x = level;
          break;
        case Wave::Rest: x = 0; break;
      }
      out.push_back(static_cast<int16_t>(std::lround(x * v * kPeak)));
    }
  }
  // The ends: a short ramp in and out.
  const size_t a = std::min(out.size(), static_cast<size_t>(kAttackS * rate)), z = std::min(out.size(), static_cast<size_t>(kFadeS * rate));
  for (size_t k = 0; k < a; ++k) out[k] = static_cast<int16_t>(out[k] * static_cast<float>(k) / a);
  for (size_t k = 0; k < z; ++k) out[out.size() - 1 - k] = static_cast<int16_t>(out[out.size() - 1 - k] * static_cast<float>(k) / z);
  return out;
}

}  // namespace carts
