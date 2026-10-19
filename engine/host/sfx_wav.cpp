// The carts' built-in sounds (sfx.hpp) as WAV files, to listen to on the desktop:
//   sfx_wav [DIR]     -> DIR/sfx_0_blip.wav ... sfx_7_success.wav (16 kHz mono, as on the board)
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "sfx.hpp"

namespace {
void put32(std::FILE* f, uint32_t v) { std::fwrite(&v, 4, 1, f); }
void put16(std::FILE* f, uint16_t v) { std::fwrite(&v, 2, 1, f); }
}  // namespace

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  const char* names[carts::kSfxCount] = {"blip", "coin", "hit", "jump", "explosion", "select", "error", "success"};
  constexpr int kRate = 16000;
  for (int n = 0; n < carts::kSfxCount; ++n) {
    const std::vector<int16_t> pcm = carts::sfx_render(n, kRate);
    const std::string path = dir + "/sfx_" + std::to_string(n) + "_" + names[n] + ".wav";
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
      std::fprintf(stderr, "can't write %s\n", path.c_str());
      return 1;
    }
    const uint32_t bytes = static_cast<uint32_t>(pcm.size() * 2);
    std::fwrite("RIFF", 1, 4, f);
    put32(f, 36 + bytes);
    std::fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);  // PCM
    put16(f, 1);  // mono
    put32(f, kRate);
    put32(f, kRate * 2);
    put16(f, 2);
    put16(f, 16);
    std::fwrite("data", 1, 4, f);
    put32(f, bytes);
    std::fwrite(pcm.data(), 2, pcm.size(), f);
    std::fclose(f);
    std::printf("%s (%.0f ms)\n", path.c_str(), pcm.size() * 1000.0 / kRate);
  }
  return 0;
}
