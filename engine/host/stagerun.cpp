// carts::Stage on the desktop: renders the test scene band by band as the board does and writes
// frames as PPM, with the time a frame took (a desktop's, not the board's).
//   stagerun [segs] [out.ppm] [t] [scale] [jet]
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "stage.hpp"

int main(int argc, char** argv) {
  const int segs = argc > 1 ? std::atoi(argv[1]) : 32;
  const char* out = argc > 2 ? argv[2] : "stage.ppm";
  const float t = argc > 3 ? static_cast<float>(std::atof(argv[3])) : 1.0f;
  const int scale = argc > 4 ? std::atoi(argv[4]) : 2;
  const bool jet = argc > 5 && std::string(argv[5]) == "jet";  // Jet's rasteriser, for comparison
  constexpr int W = carts::Stage::kW, H = carts::Stage::kH, kBand = 32;
  carts::Stage st(scale);
  if (segs > 0) {
    st.build_rings(segs);
    st.use_jet_raster(jet);
  } else {  // every shape: a box, a sphere, a cylinder, a cone, a torus and a flat icosahedron (a d20)
    const float phi = 1.618034f;
    std::vector<float> ico = {-1, phi, 0, 1, phi, 0, -1, -phi, 0, 1, -phi, 0, 0, -1, phi, 0, 1, phi,
                              0, -1, -phi, 0, 1, -phi, phi, 0, -1, phi, 0, 1, -phi, 0, -1, -phi, 0, 1};
    for (float& v : ico) v *= 30;
    const std::vector<int> faces = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                                    3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
    const int shapes[6] = {st.box(60, 60, 60), st.sphere(36, 24), st.cylinder(30, 70, 24), st.cone(34, 70, 24), st.torus(30, 12, 24),
                           st.mesh(ico, faces, false)};
    const uint32_t cols[6] = {0xE04040, 0x40A0E0, 0x40C060, 0xE0C040, 0xC060E0, 0xF0F0F0};
    for (int i = 0; i < 6; ++i) {
      const int o = st.add(shapes[i], cols[i], 60, false);
      st.place(o, -110.0f + 110 * (i % 3), i < 3 ? 60.0f : -60.0f, 0, 30, 35 + t * 40, 0);
      if (o < 0) std::printf("%s\n", st.error().c_str());
    }
  }
  std::vector<uint16_t> pic(W * H), band(W * kBand), depth(st.work_cells());
  int drawn = 0;
  const auto t0 = std::chrono::steady_clock::now();
  constexpr int kFrames = 60;
  for (int f = 0; f < kFrames; ++f) {
    st.step(t + f / 60.0f - (kFrames - 1) / 60.0f, 0, 0);  // the last frame is at t
    st.prepare();
    st.publish();
    drawn = 0;
    for (int y = 0; y < H; y += kBand) {
      drawn += st.band(y, y + kBand, band.data(), depth.data(), 0, W / 2) + st.band(y, y + kBand, band.data(), depth.data(), W / 2, W);
      for (int i = 0; i < W * kBand; ++i) pic[y * W + i] = static_cast<uint16_t>(band[i] >> 8 | band[i] << 8);
    }
  }
  const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / kFrames;
  std::printf("%d triangles, %d queued, %d drawn in bands (a triangle in several bands counts in each); %.2f ms a frame here\n",
              st.triangles(), st.queued(), drawn, ms);
  FILE* f = std::fopen(out, "wb");
  if (!f) return 1;
  std::fprintf(f, "P6\n%d %d\n255\n", W, H);
  for (const uint16_t v : pic) {
    const unsigned char px[3] = {static_cast<unsigned char>((v >> 11) * 255 / 31), static_cast<unsigned char>((v >> 5 & 63) * 255 / 63),
                                 static_cast<unsigned char>((v & 31) * 255 / 31)};
    std::fwrite(px, 1, 3, f);
  }
  std::fclose(f);
  return 0;
}
