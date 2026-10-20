#include "stage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <vector>

#include "Jet.hpp"

#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#define STAGE_HOT IRAM_ATTR __attribute__((noinline))  // the span loop: in internal RAM (a few hundred bytes)
#else
#define STAGE_HOT
#endif

namespace carts {

using namespace Renderer;

namespace {

constexpr float kPi = 3.14159265f;

constexpr uint16_t rgb565(int r, int g, int b) {
  return static_cast<uint16_t>((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
}

// A ring (a torus around the z axis): `segs` segments around, segs / 2 around the tube, normals
// exact (unit = FIXED_POINT_SCALE), so Gouraud lighting reads it as smooth.
Object* ring(int big_r, int small_r, int segs, Material* m) {
  auto* o = new Object();
  const int tube = std::max(3, segs / 2);
  for (int i = 0; i < segs; ++i) {
    const float u = 2.0f * kPi * i / segs;
    for (int j = 0; j < tube; ++j) {
      const float v = 2.0f * kPi * j / tube;
      const float nx = std::cos(v) * std::cos(u), ny = std::cos(v) * std::sin(u), nz = std::sin(v);
      Object::Vertex vx;
      vx.position = {static_cast<int32_t>(std::lround((big_r + small_r * std::cos(v)) * std::cos(u))),
                     static_cast<int32_t>(std::lround((big_r + small_r * std::cos(v)) * std::sin(u))),
                     static_cast<int32_t>(std::lround(small_r * nz))};
      vx.normal = {static_cast<int32_t>(std::lround(nx * FIXED_POINT_SCALE)), static_cast<int32_t>(std::lround(ny * FIXED_POINT_SCALE)),
                   static_cast<int32_t>(std::lround(nz * FIXED_POINT_SCALE))};
      o->addVertex(vx);
    }
  }
  for (int i = 0; i < segs; ++i)
    for (int j = 0; j < tube; ++j) {
      const int a = i * tube + j, b = ((i + 1) % segs) * tube + j;
      const int c = ((i + 1) % segs) * tube + (j + 1) % tube, d = i * tube + (j + 1) % tube;
      o->addFace(a, b, c, d, m);
    }
  o->calculateBoundingBox();
  return o;
}

// Jet's lighting model (Renderer.cpp's jetShadeBrightness and jetModulateRGB565), split in two: the
// light-dependent part (lambert, and the view-facing term the specular scales) and the material's
// (brightness()), both per vertex (an object has one material); the color by brightness is the
// material's table (make_lut), looked up per pixel.
struct Lit {
  uint16_t lambert;  // 0..255: Jet's squared Lambert, times the light's intensity
  uint16_t facing;   // 0..255: Jet's view-facing specular lobe, gated by lambert and intensity
};

Lit light_vertex(float nx, float ny, float nz, const float* l, uint32_t intensity) {
  const float d = nx * l[0] + ny * l[1] + nz * l[2];
  if (d <= 0) return {0, 0};
  uint32_t lambert = std::min<uint32_t>(static_cast<uint32_t>(d * 256.0f), 255);  // Jet: (N.L at 1024 scale) >> 12
  lambert = (lambert * lambert + 128) >> 8;
  lambert = (lambert * intensity) >> 8;
  uint32_t facing = 0;
  if (nz < 0 && lambert > 0) {
    const uint32_t vf8 = std::min<uint32_t>(static_cast<uint32_t>(-nz * 256.0f), 255);
    facing = (((vf8 * vf8) >> 8) * lambert) >> 8;
    facing = (facing * intensity) >> 8;
  }
  return {static_cast<uint16_t>(lambert), static_cast<uint16_t>(facing)};
}

// A surface: an RGB565 color, a shine (Jet's specular, 0..255) and unlit (the color as is).
struct Mat {
  uint16_t color;
  uint8_t shine;
  bool unlit;
  int16_t tex = -1;  // a texture's colors instead of one (its color unused)
  bool operator==(const Mat& o) const { return color == o.color && shine == o.shine && unlit == o.unlit && tex == o.tex; }
};

float brightness(const Lit& v, const Mat& m) {
  if (m.unlit) return 255.0f;
  const uint32_t max_b = 255u + m.shine;
  const uint32_t diffuse = (static_cast<uint32_t>(v.lambert) * 255) >> 8;  // Jet's diffuse coefficient, 255
  const uint32_t specular = (static_cast<uint32_t>(v.facing) * m.shine) >> 8;
  return static_cast<float>(std::min(diffuse + specular, max_b));
}

uint16_t modulate(uint16_t color, uint32_t b, const Color& amb, uint32_t max_b) {
  auto channel = [](uint32_t base, uint32_t top, uint32_t t) -> uint32_t {
    if (t > 255) return base + ((top - base) * (t - 255)) / 256u;
    const uint32_t v = base * t;
    return (v + 128u + (v >> 8)) >> 8;
  };
  const uint32_t r = channel(color >> 11 & 31, 31, std::min(b + amb.r, max_b));
  const uint32_t g = channel(color >> 5 & 63, 63, std::min(b + amb.g, max_b));
  const uint32_t bl = channel(color & 31, 31, std::min(b + amb.b, max_b));
  return static_cast<uint16_t>(r << 11 | g << 5 | bl);
}

constexpr uint16_t panel(uint16_t c) { return static_cast<uint16_t>(c >> 8 | c << 8); }

// A material's colors by brightness, in the panel's byte order, with kLutPad entries either side
// (rounding can step a pixel's brightness a little past its vertices').
constexpr int kLutPad = 8, kLutSize = 512 + 2 * kLutPad;
void make_lut(uint16_t* c, const Mat& m, const Color& amb) {
  const uint32_t max_b = 255u + m.shine;
  for (int i = 0; i < kLutSize; ++i) {
    const uint32_t b = static_cast<uint32_t>(std::clamp(i - kLutPad, 0, static_cast<int>(max_b)));
    c[i] = panel(m.unlit ? m.color : modulate(m.color, b, amb, max_b));
  }
}

// A textured material's colors: its texture's 16 at each of 64 brightness levels (8 apart: Jet's
// 0..511), a level either side for rounding: row L + 1 holds level L, 16 entries a row. A pixel's
// entry: (brightness >> 3) * 16 + its texel, from row 0 (the table + 16).
constexpr int kTexLevels = 64, kTexLutSize = 16 * (kTexLevels + 2);
void make_tex_lut(uint16_t* c, const Mat& m, const Color& amb, const uint16_t* pal) {
  const uint32_t max_b = 255u + m.shine;
  for (int row = 0; row < kTexLevels + 2; ++row) {
    const uint32_t b = static_cast<uint32_t>(std::clamp((row - 1) * 8 + 4, 0, static_cast<int>(max_b)));
    for (int k = 0; k < 16; ++k) c[row * 16 + k] = panel(m.unlit ? pal[k] : modulate(pal[k], b, amb, max_b));
  }
}

constexpr uint16_t rgb565_of(uint32_t rgb) { return rgb565(rgb >> 16 & 255, rgb >> 8 & 255, rgb & 255); }

// 3x3 rotations (row-major), Jet's conventions: an object turns Rz * Ry * Rx (x first), the
// camera Rz * Rx * Ry (yaw first), angles in degrees.
struct M3 {
  float m[9];
  static M3 rx(float deg) {
    const float c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    return {{1, 0, 0, 0, c, -s, 0, s, c}};
  }
  static M3 ry(float deg) {
    const float c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    return {{c, 0, s, 0, 1, 0, -s, 0, c}};
  }
  static M3 rz(float deg) {
    const float c = std::cos(deg * kPi / 180), s = std::sin(deg * kPi / 180);
    return {{c, -s, 0, s, c, 0, 0, 0, 1}};
  }
  M3 operator*(const M3& o) const {
    M3 r;
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) r.m[i * 3 + j] = m[i * 3] * o.m[j] + m[i * 3 + 1] * o.m[3 + j] + m[i * 3 + 2] * o.m[6 + j];
    return r;
  }
  inline __attribute__((always_inline)) void apply(const float* v, float* out) const {
    for (int i = 0; i < 3; ++i) out[i] = m[i * 3] * v[0] + m[i * 3 + 1] * v[1] + m[i * 3 + 2] * v[2];
  }
};

// A transformed vertex: screen position in 1/16 pixels, depth (z * 16), and its lighting.
struct V {
  int32_t x, y;
  float z;  // < 0: in front of the near plane (its triangles are dropped)
  float b;  // brightness, the node's material's (Jet's 0..255 + shine)
};

// A triangle ready for the span loop, made on core 1 as the frame is prepared. Positions in 1/16
// pixels, sorted by y; a pixel is drawn when its center is inside (left and top edges inclusive,
// right and bottom exclusive). Each edge is walked from its upper end with its own slope, so two
// triangles sharing an edge compute the same x on every row: no gaps, no double-drawn seams.
// 64 bytes (the frame's triangles are written and read back through PSRAM's cache): what the span
// loop reads, nothing more.
struct Tri {
  int32_t x0, y0, x1, y1;  // v0 (the long edge's and first short edge's upper end) and v1 (the second's)
  int32_t s02, s01, s12;   // x per unit of y along each edge, 16.16
  int32_t zs, dzx, dzy;    // depth (z * 16, 24.8) at (xb, row r0)'s center, and per pixel / row
  int32_t bs, dbx, dby;    // brightness (16.16) likewise
  int16_t r0, r1, r2;      // rows [r0, r1) use the v0-v1 edge, [r1, r2) the v1-v2 edge
  int16_t xb;              // its leftmost pixel
  uint8_t mat;             // its material (the frame's color table)
  uint8_t flags;           // kLongLeft: the v0-v2 edge is the left end of every row; kFlat: one brightness; kTextured
  uint16_t tex;            // textured: its TexRec in the frame
};
constexpr uint8_t kLongLeft = 1, kFlat = 2, kTextured = 4;

// A textured triangle's texture coordinates (texels, 16.16; v times the width, so a texel's index is
// (v >> 16 & vm) | (u >> 16 & um)) at (xb, row r0)'s center and per pixel / row, affine across it
// (the PS1's mapping), and its texels (powers of two each way: they repeat).
struct TexRec {
  uint32_t us;
  int32_t dux, duy;
  uint32_t vs;
  int32_t dvx, dvy;
  const uint8_t* texels;
  uint16_t um, vm;
};

// A texture: color indices into its palette (as RGB565), w = 1 << wbits by h = 1 << hbits.
struct Texture {
  int wbits, hbits;
  std::vector<uint8_t> texels;
  uint16_t pal[16];
};
static_assert(sizeof(Tri) == 64, "a triangle is one 64-byte record");

#define STAGE_INLINE inline __attribute__((always_inline))
static_assert(std::is_trivially_default_constructible_v<Tri>, "new Tri[n] leaves them unwritten");
// The S3's 32-bit divide instruction when the shifted numerator fits (edges under 2048 pixels
// wide), else single precision (a 64-bit or a float divide is a library call). Deterministic: both
// triangles on an edge compute it from the same two vertices.
STAGE_INLINE int32_t slope(int xa, int ya, int xb, int yb) {
  if (yb <= ya) return 0;
  const int dx = xb - xa;
  if (dx > -32768 && dx < 32768) return static_cast<int32_t>((dx * 65536) / (yb - ya));
  return static_cast<int32_t>(static_cast<float>(dx) * 65536.0f / static_cast<float>(yb - ya));
}
STAGE_INLINE int32_t round_fx(float v) { return static_cast<int32_t>(v >= 0 ? v + 0.5f : v - 0.5f); }  // not lroundf: a library call
STAGE_INLINE int ceil16(int v) { return (v + 15) >> 4; }  // ceil(v / 16), v in 1/16 pixels (arithmetic shift)
// a / b, correctly rounded. On the S3 in the FPU's divide steps (the sequence libgcc's __divsf3 has
// for it); the toolchain's own a / b calls the ROM's division, a call per divide.
STAGE_INLINE float fdiv(float a, float b) {
#if defined(ESP_PLATFORM) && defined(__XTENSA__) && CONFIG_IDF_TARGET_ESP32S3
  float q, y, e, t, z, d, n, m;
  asm("div0.s %[y], %[b]\n\t"
      "nexp01.s %[e], %[b]\n\t"
      "const.s %[t], 1\n\t"
      "maddn.s %[t], %[e], %[y]\n\t"
      "mov.s %[z], %[y]\n\t"
      "mov.s %[d], %[b]\n\t"
      "nexp01.s %[n], %[a]\n\t"
      "maddn.s %[z], %[t], %[z]\n\t"
      "const.s %[t], 1\n\t"
      "const.s %[q], 0\n\t"
      "neg.s %[m], %[n]\n\t"
      "maddn.s %[t], %[e], %[z]\n\t"
      "maddn.s %[q], %[m], %[y]\n\t"
      "mkdadj.s %[d], %[a]\n\t"
      "maddn.s %[z], %[t], %[z]\n\t"
      "maddn.s %[m], %[e], %[q]\n\t"
      "const.s %[y], 1\n\t"
      "maddn.s %[y], %[e], %[z]\n\t"
      "maddn.s %[q], %[m], %[z]\n\t"
      "neg.s %[n], %[n]\n\t"
      "maddn.s %[z], %[y], %[z]\n\t"
      "maddn.s %[n], %[e], %[q]\n\t"
      "addexpm.s %[q], %[d]\n\t"
      "addexp.s %[z], %[d]\n\t"
      "divn.s %[q], %[n], %[z]"
      : [q] "=&f"(q), [y] "=&f"(y), [e] "=&f"(e), [t] "=&f"(t), [z] "=&f"(z), [d] "=&f"(d), [n] "=&f"(n), [m] "=&f"(m)
      : [a] "f"(a), [b] "f"(b));
  return q;
#else
  return a / b;
#endif
}

constexpr int kSub = 16;            // vertex positions: 1/16 pixel
int g_prep_skip = 0;  // selftest's timing: 1 skips emit(), 2 the whole triangle loop
constexpr int kAlignCells = 8;      // work cells' slack: the depth rows moved to the color rows' alignment (band())
constexpr float kMaxCoord = 30000;  // pixels: farther vertices are clamped (int64 edges, int32 positions)

}  // namespace

// A mesh: vertices and their normals (unit), triangles (indices; (b - a) x (c - a) points out).
struct Mesh {
  std::vector<float> p, n, uv;  // (uv: a texture's coordinates, 0..1 across it, 2 a vertex)
  std::vector<uint16_t> t;
  float c[3] = {0, 0, 0}, r = 0;  // a sphere around it (add_mesh)
  std::vector<float> fp;          // each face's plane: its normal ((b - a) x (c - a), out) and n . a
};

// One frame's worth of state. Two of them: core 1 prepares the next frame into one while the
// sender draws the other's bands on core 0. The meshes are shared (preparing only reads them);
// drawing reads only the frame's own triangles and color tables. The Jet scene, camera and sun
// serve the comparison path (Jet's own pipeline and rasteriser).
struct Frame {
  Scene scene;
  Camera camera;
  DirectionalLight sun{Vector3{-40, 35, 0}, Color{255, 244, 228}, 255};
  std::vector<std::vector<int32_t>> bins;  // each piece's triangles, in draw order
  // Ours: the frame's triangles, set up (bins index these). Not a vector: its elements would be
  // zeroed as they're added (6% of a frame's preparing), and every field is written anyway.
  std::unique_ptr<Tri[]> tris;
  int ntris = 0, cap = 0;
  // Each material's colors (kLutSize), made for this frame's lighting (lut_key: what they were made for).
  std::vector<std::unique_ptr<uint16_t[]>> luts;
  std::vector<uint64_t> lut_key;
  std::vector<TexRec> texrecs;  // its textured triangles' (Tri::tex)
  std::vector<uint16_t> sky;  // its background, a rendered row each (the panel's byte order)
  uint32_t sky_version = 0;
  int scale, rw, rh;  // drawn at: 1 full size, 2 half (each pixel sent as 2x2); its rendered size
  Tri& add() {
    if (ntris == cap) {
      cap = std::max(1024, cap * 2);
      std::unique_ptr<Tri[]> more(new Tri[cap]);
      std::memcpy(more.get(), tris.get(), sizeof(Tri) * ntris);
      tris = std::move(more);
    }
    return tris[ntris++];
  }
  void resize(int s, int w, int h) {  // to another scale (its triangles are remade each frame anyway)
    scale = s;
    rw = w;
    rh = h;
    bins.assign(h / Stage::kPiece, {});
    sky.assign(h, 0);
    sky_version = 0;
  }
  Frame(int s, int w, int h) : scene(nullptr, nullptr, w, h), bins(h / Stage::kPiece), sky(h), scale(s), rw(w), rh(h) {
    scene.setClearBuffer(false);  // band() clears its own rows
    scene.setCamera(&camera);
    scene.setDirectionalLight(&sun);
  }
};

// Jet's trig tables, before anything (a Camera, a light) can read them.
struct TrigTables {
  TrigTables() { initializeTrigTables(); }
};

// An object: a mesh placed with a material. euler: the angles place() gave (the comparison path's).
struct Node {
  int mesh, mat;
  uint32_t rgb = 0;  // its color, shine and unlit (its material, with tex)
  int shine = 0;
  bool unlit = false;
  int tex = -1;              // skin()'s texture, repeated ru x rv times across the shape's (u, v)
  float ru = 1, rv = 1;
  float pos[3] = {0, 0, 0};
  M3 rot = {{1, 0, 0, 0, 1, 0, 0, 0, 1}};
  float euler[3] = {0, 0, 0};
  bool hidden = false, shadow = false;
  Object* jet = nullptr;  // the test scene's Jet copy (the comparison path)
};

M3 euler_m3(float rx, float ry, float rz) { return M3::rz(rz) * M3::ry(ry) * M3::rx(rx); }

struct Stage::Impl {
  TrigTables trig;
  const int out_w;  // output width
  int scale, rw, rh;  // what the next prepare() draws at, and its rendered size (each frame has its own)
  int want_scale;     // set_scale()'s, from the next prepare()
  bool auto_res = false;
  const float center;             // the window's center in the output (the projection's)
  float fov_deg = 60, near = 2, far = 4000;  // depth is stored as z * 16 in 16 bits
  float cam_pos[3] = {0, 0, -420}, yaw = 0, pitch = 0;
  float light_dir[3];  // world, toward the sun
  uint32_t intensity = 255;
  Color amb{34, 40, 62};
  uint32_t sky_top = 0x16224E, sky_bottom = 0x060812;
  float ground_y = 0;
  int shadow_mat = -1;  // the shadows' material (unlit), once ground() is called
  uint32_t scene_version = 1;  // light or sky changed: the frames' color tables and sky are remade
  Frame frames[2];
  int front = 0;  // the frame band() draws; prepare() fills the other
  std::vector<Mesh> meshes;
  std::vector<Mat> mats;
  std::vector<Texture> textures;
  std::vector<Node> nodes;
  std::vector<V> verts;  // the node being prepared's vertices, transformed
  std::vector<uint32_t> vstamp;  // a vertex is needed this pass when its stamp is `stamp` (a face using it faces the camera)
  uint32_t stamp = 0;
  std::vector<uint16_t> facing;  // the node's faces turned to the camera
  std::string err;
  int vertices = 0, tris = 0, queued = 0, drawn_nodes = 0;
  bool jet_raster = false;  // the test scene through Jet's pipeline and rasteriser (for comparison)
  int64_t jet_us = 0;       // the last prepare's time
  // The comparison path's
  Camera camera;
  AmbientLight jet_amb{Color{34, 40, 62}};
  std::vector<Material*> jet_mats;
  std::vector<Object*> jet_objects;

  // A scale's rendered width: even (two pixels a word), at half the output's halved and rounded up
  // (410 -> 206: the last column doubled runs past the output's edge and is clipped).
  static int rw_of(int w, int s) { return s == 1 ? w : (w + 3) / 4 * 2; }
  Impl(int s, int w, float c)
      : out_w(w), scale(s), rw(rw_of(w, s)), rh(kH / s), want_scale(s), center(c),
        frames{{s, rw_of(w, s), kH / s}, {s, rw_of(w, s), kH / s}} {
    camera.setPosition(static_cast<int32_t>(cam_pos[0]), static_cast<int32_t>(cam_pos[1]), static_cast<int32_t>(cam_pos[2]));
    camera.setFOV(fov_deg, rw);
    camera.nearPlane = 32;
    camera.farPlane = 2048;
    for (auto& f : frames) f.scene.setAmbientLight(&jet_amb);
    set_light(-40, 35, 255);
    textures.reserve(kMaxTextures);  // never moved (their texels are read by the drawing cores)
  }
  ~Impl() {
    for (auto* o : jet_objects) delete o;
    for (auto* m : jet_mats) delete m;
  }

  void set_light(float az, float el, uint32_t in) {  // Jet's DirectionalLight: x = cos el cos az, y = sin el, z = cos el sin az
    const float a = az * kPi / 180, e = el * kPi / 180;
    light_dir[0] = std::cos(e) * std::cos(a);
    light_dir[1] = std::sin(e);
    light_dir[2] = std::cos(e) * std::sin(a);
    intensity = std::min<uint32_t>(in, 255);
    ++scene_version;
  }
  bool fail(std::string why) {
    err = std::move(why);
    return false;
  }
  int add_mesh(Mesh&& m) {
    if (static_cast<int>(meshes.size()) >= kMaxMeshes) return fail("too many shapes (the limit is " + std::to_string(kMaxMeshes) + ")"), -1;
    const int nv = static_cast<int>(m.p.size() / 3), nt = static_cast<int>(m.t.size() / 3);
    if (nv == 0 || nt == 0) return fail("a shape with no triangles"), -1;
    if (vertices + nv > kMaxVertices || tris + nt > kMaxTriangles)
      return fail("the scene's shapes would hold " + std::to_string(vertices + nv) + " vertices and " + std::to_string(tris + nt) +
                  " triangles; the limits are " + std::to_string(kMaxVertices) + " and " + std::to_string(kMaxTriangles)), -1;
    vertices += nv;
    tris += nt;
    // its bounding sphere: the box's center, the farthest vertex from it
    float lo[3] = {m.p[0], m.p[1], m.p[2]}, hi[3] = {m.p[0], m.p[1], m.p[2]};
    for (int i = 0; i < nv; ++i)
      for (int k = 0; k < 3; ++k) {
        lo[k] = std::min(lo[k], m.p[i * 3 + k]);
        hi[k] = std::max(hi[k], m.p[i * 3 + k]);
      }
    float r2 = 0;
    for (int k = 0; k < 3; ++k) m.c[k] = (lo[k] + hi[k]) / 2;
    for (int i = 0; i < nv; ++i) {
      const float dx = m.p[i * 3] - m.c[0], dy = m.p[i * 3 + 1] - m.c[1], dz = m.p[i * 3 + 2] - m.c[2];
      r2 = std::max(r2, dx * dx + dy * dy + dz * dz);
    }
    m.r = std::sqrt(r2) * 1.0001f + 0.01f;
    m.fp.resize(static_cast<size_t>(nt) * 4);
    for (int k = 0; k < nt; ++k) {
      const float *a = &m.p[m.t[k * 3] * 3], *b = &m.p[m.t[k * 3 + 1] * 3], *c = &m.p[m.t[k * 3 + 2] * 3];
      const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
      float* f = &m.fp[static_cast<size_t>(k) * 4];
      f[0] = u[1] * v[2] - u[2] * v[1];
      f[1] = u[2] * v[0] - u[0] * v[2];
      f[2] = u[0] * v[1] - u[1] * v[0];
      f[3] = f[0] * a[0] + f[1] * a[1] + f[2] * a[2];
    }
    if (nv > static_cast<int>(vstamp.size())) vstamp.resize(nv, 0);
    meshes.push_back(std::move(m));
    return static_cast<int>(meshes.size()) - 1;
  }
  int mat_for(uint32_t rgb, int shine, bool unlit, int tex = -1) {
    const Mat m{tex >= 0 ? uint16_t{0} : rgb565_of(rgb), static_cast<uint8_t>(std::clamp(shine, 0, 255)), unlit, static_cast<int16_t>(tex)};
    for (size_t i = 0; i < mats.size(); ++i)
      if (mats[i] == m) return static_cast<int>(i);
    if (static_cast<int>(mats.size()) >= kMaxMaterials)
      return fail("too many colors in the scene (the limit is " + std::to_string(kMaxMaterials) + " color and shine pairs)"), -1;
    mats.push_back(m);
    return static_cast<int>(mats.size()) - 1;
  }
  Node* node(int id) {
    if (id < 0 || id >= static_cast<int>(nodes.size())) return fail("no object " + std::to_string(id)), nullptr;
    return &nodes[id];
  }
  M3 view_m3() const { return M3::rx(pitch) * M3::ry(-yaw); }

  void prepare_jet(Frame& f);
  void prepare_ours(Frame& f);
  // uv: the vertices' texture coordinates (texels: u, v for a, b, c) when tex is the material's texture
  template <bool kTex>
  void emit_t(Frame& f, const V& a, const V& b, const V& c, int mat, const float* uv, const Texture* tex);
  void emit(Frame& f, const V& a, const V& b, const V& c, int mat) { emit_t<false>(f, a, b, c, mat, nullptr, nullptr); }
  void emit(Frame& f, const V& a, const V& b, const V& c, int mat, const float* uv, const Texture* tex) { emit_t<true>(f, a, b, c, mat, uv, tex); }
};

Stage::Stage(int scale, int width, float center_x) : p_(new Impl(scale == 2 ? 2 : 1, width, center_x)) {}
Stage::~Stage() = default;

// ---- meshes ----

namespace {

// Appends a triangle, turned so (b - a) x (c - a) points along `out` (the outward direction).
void tri_out(Mesh& m, int a, int b, int c, const float* out) {
  const float* p = m.p.data();
  const float u[3] = {p[b * 3] - p[a * 3], p[b * 3 + 1] - p[a * 3 + 1], p[b * 3 + 2] - p[a * 3 + 2]};
  const float v[3] = {p[c * 3] - p[a * 3], p[c * 3 + 1] - p[a * 3 + 1], p[c * 3 + 2] - p[a * 3 + 2]};
  const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
  if (n[0] * out[0] + n[1] * out[1] + n[2] * out[2] < 0) std::swap(b, c);
  m.t.push_back(static_cast<uint16_t>(a));
  m.t.push_back(static_cast<uint16_t>(b));
  m.t.push_back(static_cast<uint16_t>(c));
}

int vert(Mesh& m, float x, float y, float z, float nx, float ny, float nz, float u = 0, float v = 0) {
  m.p.insert(m.p.end(), {x, y, z});
  m.uv.insert(m.uv.end(), {u, v});
  const float l = std::sqrt(nx * nx + ny * ny + nz * nz);
  const float k = l > 0 ? 1 / l : 0;
  m.n.insert(m.n.end(), {nx * k, ny * k, nz * k});
  return static_cast<int>(m.p.size() / 3) - 1;
}

// A flat quad from four corners (in order around it), facing `out`.
void quad(Mesh& m, const float* c0, const float* c1, const float* c2, const float* c3, const float* out) {
  const int a = vert(m, c0[0], c0[1], c0[2], out[0], out[1], out[2], 0, 1), b = vert(m, c1[0], c1[1], c1[2], out[0], out[1], out[2], 1, 1);
  const int c = vert(m, c2[0], c2[1], c2[2], out[0], out[1], out[2], 1, 0), d = vert(m, c3[0], c3[1], c3[2], out[0], out[1], out[2], 0, 0);
  tri_out(m, a, b, c, out);
  tri_out(m, a, c, d, out);
}

}  // namespace

int Stage::box(float w, float h, float d) {
  Mesh m;
  const float x = w / 2, y = h / 2, z = d / 2;
  for (int axis = 0; axis < 3; ++axis)
    for (int sgn = -1; sgn <= 1; sgn += 2) {
      float out[3] = {0, 0, 0};
      out[axis] = static_cast<float>(sgn);
      const int u = (axis + 1) % 3, v = (axis + 2) % 3;
      const float ext[3] = {x, y, z};
      float c[4][3];
      const int su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
      for (int k = 0; k < 4; ++k) {
        c[k][axis] = sgn * ext[axis];
        c[k][u] = su[k] * ext[u];
        c[k][v] = sv[k] * ext[v];
      }
      quad(m, c[0], c[1], c[2], c[3], out);
    }
  return p_->add_mesh(std::move(m));
}

int Stage::sphere(float r, int segs) {
  segs = std::clamp(segs, 4, 64);
  const int rings = std::max(2, segs / 2);
  Mesh m;
  for (int i = 0; i <= rings; ++i) {
    const float lat = kPi * i / rings;
    for (int j = 0; j <= segs; ++j) {
      const float lon = 2 * kPi * j / segs;
      const float nx = std::sin(lat) * std::cos(lon), ny = std::cos(lat), nz = std::sin(lat) * std::sin(lon);
      vert(m, r * nx, r * ny, r * nz, nx, ny, nz, static_cast<float>(j) / segs, static_cast<float>(i) / rings);
    }
  }
  for (int i = 0; i < rings; ++i)
    for (int j = 0; j < segs; ++j) {
      const int a = i * (segs + 1) + j, b = a + 1, c = a + segs + 1, d = c + 1;
      const float* n = &m.n[static_cast<size_t>(a) * 3];
      const float mid[3] = {n[0] + m.n[d * 3], n[1] + m.n[d * 3 + 1], n[2] + m.n[d * 3 + 2]};
      if (i > 0) tri_out(m, a, b, d, mid);
      if (i < rings - 1) tri_out(m, a, d, c, mid);
    }
  return p_->add_mesh(std::move(m));
}

int Stage::torus(float big_r, float small_r, int segs) {  // a ring around the z axis (facing the camera)
  segs = std::clamp(segs, 6, 96);
  const int tube = std::max(3, segs / 2);
  Mesh m;
  // (segs + 1) x (tube + 1) vertices: the seams' twice, at the same positions (the angles taken
  // around), with the texture's u and v running 0..1 to each seam
  for (int i = 0; i <= segs; ++i) {
    const float u = 2 * kPi * (i % segs) / segs;
    for (int j = 0; j <= tube; ++j) {
      const float v = 2 * kPi * (j % tube) / tube;
      const float nx = std::cos(v) * std::cos(u), ny = std::cos(v) * std::sin(u), nz = std::sin(v);
      vert(m, (big_r + small_r * std::cos(v)) * std::cos(u), (big_r + small_r * std::cos(v)) * std::sin(u), small_r * nz, nx, ny, nz,
           static_cast<float>(i) / segs, static_cast<float>(j) / tube);
    }
  }
  for (int i = 0; i < segs; ++i)
    for (int j = 0; j < tube; ++j) {
      const int a = i * (tube + 1) + j, b = (i + 1) * (tube + 1) + j;
      const int c = (i + 1) * (tube + 1) + j + 1, d = i * (tube + 1) + j + 1;
      const float* n = &m.n[static_cast<size_t>(a) * 3];
      tri_out(m, a, b, c, n);
      tri_out(m, a, c, d, n);
    }
  return p_->add_mesh(std::move(m));
}

namespace {
// A cylinder or cone along y (top radius rt, bottom rb), with flat caps.
void lathe(Mesh& m, float rt, float rb, float h, int segs) {
  const float y0 = -h / 2, y1 = h / 2;
  const float slope = (rb - rt) / h;  // the side's normal leans up by this
  for (int j = 0; j <= segs; ++j) {
    const float a = 2 * kPi * j / segs, c = std::cos(a), s = std::sin(a);
    vert(m, rb * c, y0, rb * s, c, slope, s, static_cast<float>(j) / segs, 1);
    vert(m, rt * c, y1, rt * s, c, slope, s, static_cast<float>(j) / segs, 0);
  }
  for (int j = 0; j < segs; ++j) {
    const int a = 2 * j, b = a + 1, c = a + 2, d = a + 3;
    const float* n = &m.n[static_cast<size_t>(a) * 3];
    if (rb > 0) tri_out(m, a, c, d, n);
    if (rt > 0) tri_out(m, a, d, b, n);
  }
  for (int cap = 0; cap < 2; ++cap) {
    const float r = cap ? rt : rb, y = cap ? y1 : y0, ny = cap ? 1.0f : -1.0f;
    if (r <= 0) continue;
    const float out[3] = {0, ny, 0};
    const int center = vert(m, 0, y, 0, 0, ny, 0, 0.5f, 0.5f);
    for (int j = 0; j <= segs; ++j) {
      const float a = 2 * kPi * j / segs;
      vert(m, r * std::cos(a), y, r * std::sin(a), 0, ny, 0, 0.5f + 0.5f * std::cos(a), 0.5f + 0.5f * std::sin(a));
    }
    for (int j = 0; j < segs; ++j) tri_out(m, center, center + 1 + j, center + 2 + j, out);
  }
}
}  // namespace

int Stage::cylinder(float r, float h, int segs) {
  Mesh m;
  lathe(m, r, r, h, std::clamp(segs, 3, 96));
  return p_->add_mesh(std::move(m));
}

int Stage::cone(float r, float h, int segs) {
  Mesh m;
  lathe(m, 0, r, h, std::clamp(segs, 3, 96));
  return p_->add_mesh(std::move(m));
}

int Stage::mesh(const std::vector<float>& xyz, const std::vector<int>& idx, bool smooth, const std::vector<float>* uv) {
  const int nv = static_cast<int>(xyz.size() / 3);
  if (xyz.size() % 3 || idx.size() % 3) return p_->fail("a shape's vertices come in threes (x, y, z) and its faces in threes (a, b, c)"), -1;
  if (uv && uv->size() != static_cast<size_t>(nv) * 2)
    return p_->fail("a shape's texture coordinates come in twos (u, v), one pair a vertex: " + std::to_string(nv * 2) + " numbers, not " +
                    std::to_string(uv->size())), -1;
  auto uv_of = [&](int i, int k) { return uv ? (*uv)[static_cast<size_t>(i) * 2 + k] : 0.0f; };
  for (const int i : idx)
    if (i < 0 || i >= nv) return p_->fail("a face uses vertex " + std::to_string(i + 1) + "; the shape has " + std::to_string(nv)), -1;
  // Faces turned outward from the shape's center (meant for convex shapes, as dice are).
  float c[3] = {0, 0, 0};
  for (int i = 0; i < nv; ++i)
    for (int k = 0; k < 3; ++k) c[k] += xyz[i * 3 + k] / nv;
  Mesh m;
  if (smooth) {  // shared vertices; normals: the faces' around each, weighted by area
    m.p = xyz;
    m.n.assign(xyz.size(), 0);
    m.uv = uv ? *uv : std::vector<float>(static_cast<size_t>(nv) * 2, 0.0f);
  }
  for (size_t f = 0; f < idx.size(); f += 3) {
    const float* a = &xyz[idx[f] * 3];
    const float* b = &xyz[idx[f + 1] * 3];
    const float* d = &xyz[idx[f + 2] * 3];
    const float u[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, v[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
    const float mid[3] = {(a[0] + b[0] + d[0]) / 3 - c[0], (a[1] + b[1] + d[1]) / 3 - c[1], (a[2] + b[2] + d[2]) / 3 - c[2]};
    if (n[0] * mid[0] + n[1] * mid[1] + n[2] * mid[2] < 0)
      for (float& x : n) x = -x;
    if (smooth) {
      for (int k = 0; k < 3; ++k)
        for (int j = 0; j < 3; ++j) m.n[idx[f + k] * 3 + j] += n[j];
      tri_out(m, idx[f], idx[f + 1], idx[f + 2], n);
    } else {
      const int i0 = vert(m, a[0], a[1], a[2], n[0], n[1], n[2], uv_of(idx[f], 0), uv_of(idx[f], 1));
      const int i1 = vert(m, b[0], b[1], b[2], n[0], n[1], n[2], uv_of(idx[f + 1], 0), uv_of(idx[f + 1], 1));
      const int i2 = vert(m, d[0], d[1], d[2], n[0], n[1], n[2], uv_of(idx[f + 2], 0), uv_of(idx[f + 2], 1));
      tri_out(m, i0, i1, i2, n);
    }
  }
  if (smooth)
    for (int i = 0; i < nv; ++i) {
      float* n = &m.n[i * 3];
      const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
      if (l > 0)
        for (int k = 0; k < 3; ++k) n[k] /= l;
    }
  return p_->add_mesh(std::move(m));
}

// ---- objects, camera, light ----

int Stage::add(int mesh, uint32_t rgb, int shine, bool unlit) {
  Impl& p = *p_;
  if (mesh < 0 || mesh >= static_cast<int>(p.meshes.size())) return p.fail("no shape " + std::to_string(mesh)), -1;
  if (static_cast<int>(p.nodes.size()) >= kMaxNodes) return p.fail("too many objects (the limit is " + std::to_string(kMaxNodes) + ")"), -1;
  const int mat = p.mat_for(rgb, shine, unlit);
  if (mat < 0) return -1;
  Node n;
  n.mesh = mesh;
  n.mat = mat;
  n.rgb = rgb;
  n.shine = shine;
  n.unlit = unlit;
  p.nodes.push_back(n);
  return static_cast<int>(p.nodes.size()) - 1;
}

bool Stage::place(int obj, float x, float y, float z, float rx, float ry, float rz) {
  Node* n = p_->node(obj);
  if (!n) return false;
  n->pos[0] = x;
  n->pos[1] = y;
  n->pos[2] = z;
  n->euler[0] = rx;
  n->euler[1] = ry;
  n->euler[2] = rz;
  n->rot = euler_m3(rx, ry, rz);
  return true;
}

bool Stage::orient(int obj, const float m[9]) {
  Node* n = p_->node(obj);
  if (!n) return false;
  std::memcpy(n->rot.m, m, sizeof n->rot.m);
  return true;
}

bool Stage::hide(int obj, bool hidden) {
  Node* n = p_->node(obj);
  if (!n) return false;
  n->hidden = hidden;
  return true;
}

bool Stage::recolor(int obj, uint32_t rgb, int shine, bool unlit) {
  Node* n = p_->node(obj);
  if (!n) return false;
  const int mat = p_->mat_for(rgb, shine, unlit, n->tex);
  if (mat < 0) return false;
  n->mat = mat;
  n->rgb = rgb;
  n->shine = shine;
  n->unlit = unlit;
  return true;
}

int Stage::texture(int w, int h, const std::vector<uint8_t>& texels, const uint32_t palette[16]) {
  Impl& p = *p_;
  auto bits = [](int v) {  // log2 of a power of two from 4 to 64, else -1
    for (int b = 2; b <= 6; ++b)
      if (v == 1 << b) return b;
    return -1;
  };
  const int wb = bits(w), hb = bits(h);
  if (wb < 0 || hb < 0) return p.fail("a texture is 4, 8, 16, 32 or 64 texels wide and high (not " + std::to_string(w) + " x " + std::to_string(h) + ")"), -1;
  if (texels.size() != static_cast<size_t>(w) * h) return p.fail("a texture of " + std::to_string(w) + " x " + std::to_string(h) + " needs " + std::to_string(w * h) + " texels"), -1;
  if (static_cast<int>(p.textures.size()) >= kMaxTextures) return p.fail("too many textures (the limit is " + std::to_string(kMaxTextures) + ")"), -1;
  Texture t;
  t.wbits = wb;
  t.hbits = hb;
  t.texels = texels;
  for (uint8_t& v : t.texels) v &= 15;
  for (int k = 0; k < 16; ++k) t.pal[k] = rgb565_of(palette[k]);
  p.textures.push_back(std::move(t));
  return static_cast<int>(p.textures.size()) - 1;
}

bool Stage::skin(int obj, int tex, float ru, float rv) {
  Impl& p = *p_;
  Node* n = p.node(obj);
  if (!n) return false;
  if (tex >= static_cast<int>(p.textures.size())) return p.fail("no texture " + std::to_string(tex + 1)), false;
  const int mat = p.mat_for(n->rgb, n->shine, n->unlit, tex < 0 ? -1 : tex);
  if (mat < 0) return false;
  n->mat = mat;
  n->tex = tex < 0 ? -1 : tex;
  n->ru = ru;
  n->rv = rv;
  return true;
}

void Stage::view(float x, float y, float z, float yaw, float pitch, float fov) {
  Impl& p = *p_;
  p.cam_pos[0] = x;
  p.cam_pos[1] = y;
  p.cam_pos[2] = z;
  p.yaw = yaw;
  p.pitch = pitch;
  p.fov_deg = std::clamp(fov, 10.0f, 150.0f);
}

void Stage::light(float azimuth, float elevation, int intensity, uint32_t ambient_rgb) {
  p_->set_light(azimuth, elevation, static_cast<uint32_t>(std::clamp(intensity, 0, 255)));
  p_->amb = Color{static_cast<uint8_t>(ambient_rgb >> 16 & 255), static_cast<uint8_t>(ambient_rgb >> 8 & 255),
                  static_cast<uint8_t>(ambient_rgb & 255)};
}

void Stage::sky(uint32_t top_rgb, uint32_t bottom_rgb) {
  p_->sky_top = top_rgb;
  p_->sky_bottom = bottom_rgb;
  ++p_->scene_version;
}

void Stage::ground(float height, uint32_t rgb) {
  p_->ground_y = height;
  const int m = p_->mat_for(rgb, 0, true);
  if (m >= 0) p_->shadow_mat = m;
}

bool Stage::shadow(int obj, bool on) {
  Node* n = p_->node(obj);
  if (!n) return false;
  n->shadow = on;
  return true;
}

bool Stage::project(float x, float y, float z, float* sx, float* sy, float* depth) const {
  const Impl& p = *p_;
  const float rel[3] = {x - p.cam_pos[0], y - p.cam_pos[1], z - p.cam_pos[2]};
  float q[3];
  p.view_m3().apply(rel, q);
  if (q[2] <= p.near) return false;
  const float focal = (kW * 0.5f) / std::tan(p.fov_deg * kPi / 360.0f);  // fov spans the window
  *sx = q[0] * focal / q[2] + p.center;
  *sy = kH * 0.5f - q[1] * focal / q[2];
  *depth = q[2];
  return true;
}

const std::string& Stage::error() const { return p_->err; }
int Stage::objects() const { return static_cast<int>(p_->nodes.size()); }

// ---- the test scene ----

void Stage::build_rings(int segs) {
  Impl& p = *p_;
  const int a = torus(80, 28, segs), b = torus(80, 28, segs);
  add(a, 0xFFAA3C, 40, false);
  add(b, 0x28D2C8, 40, false);
  // The comparison path's copies, through Jet
  auto* gold = new Material(rgb565(255, 170, 60));
  gold->shadingMode = ShadingMode::GOURAUD;
  gold->specular = 40;
  auto* teal = new Material(rgb565(40, 210, 200));
  teal->shadingMode = ShadingMode::GOURAUD;
  teal->specular = 40;
  p.jet_mats = {gold, teal};
  p.jet_objects = {ring(80, 28, segs, gold), ring(80, 28, segs, teal)};
  for (int i = 0; i < 2; ++i) {
    for (auto& f : p.frames) f.scene.addObject(p.jet_objects[i]);
    p.nodes[i].jet = p.jet_objects[i];
  }
}

void Stage::step(float t, float drag_x, float drag_y) {
  if (p_->nodes.size() < 2) return;
  // The pair turns as one: a links into b at right angles, offset along its own x.
  const float spin = t * 40.0f + drag_x * 0.5f;
  const float tip = 25.0f + 15.0f * std::sin(t * 0.7f) + drag_y * 0.5f;
  const float ry = spin * kPi / 180.0f;
  place(0, -40 * std::cos(ry), 0, -40 * std::sin(ry), tip, spin, 0);
  place(1, 40 * std::cos(ry), 0, 40 * std::sin(ry), tip + 90, spin, 0);
}

// ---- preparing ----

void Stage::Impl::prepare_jet(Frame& f) {
  for (const Node& n : nodes) {  // Jet's whole degrees and units
    if (!n.jet) continue;
    auto deg = [](float d) { return (static_cast<int>(std::lround(d)) % 360 + 360) % 360; };
    n.jet->setRotation(deg(n.euler[0]), deg(n.euler[1]), deg(n.euler[2]));
    n.jet->setPosition(static_cast<int32_t>(std::lround(n.pos[0])), static_cast<int32_t>(std::lround(n.pos[1])),
                       static_cast<int32_t>(std::lround(n.pos[2])));
  }
  f.camera = camera;
  f.scene.prepareFrame();
  f.scene.advanceFrameCounter();
  queued = f.scene.lastFrameDrawnTriangles;
  for (const int32_t idx : f.scene.drawOrder()) {
    int32_t lo, hi;
    f.scene.triangleRows(idx, lo, hi);
    if (hi < 0 || lo >= rh) continue;
    const int b0 = std::max<int32_t>(lo, 0) / kPiece, b1 = std::min<int32_t>(hi, rh - 1) / kPiece;
    for (int b = b0; b <= b1; ++b) f.bins[b].push_back(idx);
  }
}

// Ours: each vertex transformed, projected and lit once; each front-facing triangle set up for the
// span loop and binned by the pieces its rows cross. No sort: the depth buffer orders the pixels.
void Stage::Impl::prepare_ours(Frame& f) {
  f.ntris = 0;
  drawn_nodes = 0;
  // The frame's color tables and sky, remade when the light, the sky or the materials changed.
  f.luts.resize(mats.size());
  f.lut_key.resize(mats.size(), 0);
  for (size_t i = 0; i < mats.size(); ++i) {
    const Mat& m = mats[i];
    const uint64_t key = static_cast<uint64_t>(scene_version) << 32 |
                         (m.color | m.shine << 16 | (m.unlit ? 1u : 0u) << 24 | static_cast<uint32_t>(m.tex + 1) << 25);
    if (f.luts[i] && f.lut_key[i] == key) continue;
    if (!f.luts[i]) f.luts[i].reset(new uint16_t[m.tex >= 0 ? kTexLutSize : kLutSize]);  // (a material's kind never changes)
    if (m.tex >= 0) make_tex_lut(f.luts[i].get(), m, amb, textures[m.tex].pal);
    else make_lut(f.luts[i].get(), m, amb);
    f.lut_key[i] = key;
  }
  f.texrecs.clear();
  const M3 cam = view_m3();
  float l[3];
  cam.apply(light_dir, l);
  const float focal = fdiv(static_cast<float>(kW) / scale * 0.5f, std::tan(fov_deg * kPi / 360.0f));  // Jet's fovFactor; fov spans the window
  const float cx = center / scale, cy = rh * 0.5f;
  // The view's four side planes, for objects' bounding spheres: what shows has x / z in [xl, xr] and
  // y / z in [yb, yt]; a sphere is out past a plane when its center is more than r beyond it (the
  // planes' normals' lengths: sqrt(1 + slope^2)).
  const float xl = fdiv(-cx, focal), xr = fdiv(rw - cx, focal), yt = fdiv(cy, focal), yb = fdiv(cy - rh, focal);
  const float kl = std::sqrt(1 + xl * xl), kr = std::sqrt(1 + xr * xr), kt = std::sqrt(1 + yt * yt), kb = std::sqrt(1 + yb * yb);
  auto project = [&](const float* q, V& v) {  // a view-space point to the screen (v.b left to the caller)
    if (q[2] < near || q[2] > far) {
      v.z = -1;
      return false;
    }
    const float inv = fdiv(focal, q[2]);
    v.x = round_fx(std::clamp(q[0] * inv + cx, -kMaxCoord, kMaxCoord) * kSub);
    v.y = round_fx(std::clamp(cy - q[1] * inv, -kMaxCoord, kMaxCoord) * kSub);
    v.z = std::min(q[2] * 16.0f, 65534.0f);
    return true;
  };
  for (const Node& n : nodes) {
    if (n.hidden) continue;
    const Mesh& o = meshes[n.mesh];
    const M3 a = cam * n.rot;
    const float rel[3] = {n.pos[0] - cam_pos[0], n.pos[1] - cam_pos[1], n.pos[2] - cam_pos[2]};
    float t[3];
    cam.apply(rel, t);
    const size_t nv = o.p.size() / 3;
    const uint16_t* tri = o.t.data();
    // In view at all? (Its shadow may be, though it isn't: that's checked by its own vertices.)
    float sc[3];
    a.apply(o.c, sc);
    for (int k = 0; k < 3; ++k) sc[k] += t[k];
    const bool out = sc[2] + o.r < near || sc[2] - o.r > far || sc[0] - xl * sc[2] < -o.r * kl || xr * sc[2] - sc[0] < -o.r * kr ||
                     yt * sc[2] - sc[1] < -o.r * kt || sc[1] - yb * sc[2] < -o.r * kb;
    if (!out) {
      ++drawn_nodes;
      verts.resize(nv);
      V* vp = verts.data();
      const Mat& m = mats[n.mat];
      // Wholly in view (the sphere inside every plane, with a pixel to spare): no vertex is clipped or
      // clamped, so the loop skips those tests.
      const bool inside = sc[2] - o.r >= near && sc[2] + o.r <= far && sc[0] - xl * sc[2] > o.r * kl && xr * sc[2] - sc[0] > o.r * kr &&
                          yt * sc[2] - sc[1] > o.r * kt && sc[1] - yb * sc[2] > o.r * kb;
      const float m0 = a.m[0], m1 = a.m[1], m2 = a.m[2], m3 = a.m[3], m4 = a.m[4], m5 = a.m[5], m6 = a.m[6], m7 = a.m[7], m8 = a.m[8];
      const float t0 = t[0], t1 = t[1], t2 = t[2], fx = focal * kSub, ox = cx * kSub, oy = cy * kSub;
      // The faces turned to the camera: the camera, in the object's own space, on a face's outer side.
      // (As the screen's winding would say, without the vertices: those only back faces use aren't
      // transformed at all.)
      float e[3];
      {
        const float* r = n.rot.m;
        const float d[3] = {cam_pos[0] - n.pos[0], cam_pos[1] - n.pos[1], cam_pos[2] - n.pos[2]};
        const float c0 = r[4] * r[8] - r[5] * r[7], c1 = r[5] * r[6] - r[3] * r[8], c2 = r[3] * r[7] - r[4] * r[6];
        const float det = r[0] * c0 + r[1] * c1 + r[2] * c2;
        if (det == 0) continue;  // flattened to nothing
        const float k = fdiv(1.0f, det);  // the inverse's rows: the adjugate's, over det
        e[0] = (c0 * d[0] + (r[2] * r[7] - r[1] * r[8]) * d[1] + (r[1] * r[5] - r[2] * r[4]) * d[2]) * k;
        e[1] = (c1 * d[0] + (r[0] * r[8] - r[2] * r[6]) * d[1] + (r[2] * r[3] - r[0] * r[5]) * d[2]) * k;
        e[2] = (c2 * d[0] + (r[1] * r[6] - r[0] * r[7]) * d[1] + (r[0] * r[4] - r[1] * r[3]) * d[2]) * k;
      }
      if (++stamp == 0) {  // wrapped: every stamp is stale again
        std::fill(vstamp.begin(), vstamp.end(), 0);
        stamp = 1;
      }
      const uint32_t st = stamp;
      uint32_t* vs = vstamp.data();
      facing.resize(o.t.size() / 3);
      uint16_t* fr = facing.data();
      int nfront = 0;
      {
        const float* fp = o.fp.data();
        const float e0 = e[0], e1 = e[1], e2 = e[2];
        const int nt = static_cast<int>(o.t.size() / 3);
        for (int k = 0; k < nt; ++k, fp += 4)
          if (fp[0] * e0 + fp[1] * e1 + fp[2] * e2 > fp[3]) {
            fr[nfront++] = static_cast<uint16_t>(k);
            vs[tri[k * 3]] = vs[tri[k * 3 + 1]] = vs[tri[k * 3 + 2]] = st;
          }
      }
      if (g_prep_skip == 2) continue;
      const float* pp = o.p.data();
      const float* np = o.n.data();
      const bool lit = !m.unlit;
      for (size_t i = 0; i < nv; ++i, pp += 3, np += 3) {
        if (vs[i] != st) continue;
        const float px = pp[0], py = pp[1], pz = pp[2];
        const float qz = m6 * px + m7 * py + m8 * pz + t2;
        V& v = vp[i];
        if (!inside && (qz < near || qz > far)) {
          v.z = -1;
          continue;
        }
        const float qx = m0 * px + m1 * py + m2 * pz + t0, qy = m3 * px + m4 * py + m5 * pz + t1;
        const float inv = fdiv(fx, qz);  // 1/16 px per unit
        float sx = qx * inv + ox, sy = oy - qy * inv;
        if (!inside) {
          sx = std::clamp(sx, -kMaxCoord * kSub, kMaxCoord * kSub);
          sy = std::clamp(sy, -kMaxCoord * kSub, kMaxCoord * kSub);
        }
        v.x = round_fx(sx);
        v.y = round_fx(sy);
        v.z = std::min(qz * 16.0f, 65534.0f);
        if (lit) {
          const float nx = m0 * np[0] + m1 * np[1] + m2 * np[2], ny = m3 * np[0] + m4 * np[1] + m5 * np[2], nz = m6 * np[0] + m7 * np[1] + m8 * np[2];
          v.b = brightness(light_vertex(nx, ny, nz, l, intensity), m);
        } else {
          v.b = 255.0f;
        }
      }
      if (g_prep_skip == 1) {
        f.ntris += nfront;
        continue;
      }
      const Texture* tx = n.tex >= 0 ? &textures[n.tex] : nullptr;
      const float tu = tx ? n.ru * static_cast<float>(1 << tx->wbits) : 0, tv = tx ? n.rv * static_cast<float>(1 << tx->hbits) : 0;
      for (int j = 0; j < nfront; ++j) {
        const uint16_t* ti = tri + fr[j] * 3;
        const V &v0 = vp[ti[0]], &v1 = vp[ti[1]], &v2 = vp[ti[2]];
        if (v0.z < 0 || v1.z < 0 || v2.z < 0) continue;
        if (tx) {  // its texture coordinates in texels
          const float* q = o.uv.data();
          const float uvs[6] = {q[ti[0] * 2] * tu, q[ti[0] * 2 + 1] * tv, q[ti[1] * 2] * tu, q[ti[1] * 2 + 1] * tv, q[ti[2] * 2] * tu, q[ti[2] * 2 + 1] * tv};
          emit(f, v0, v1, v2, n.mat, uvs, tx);
        } else {
          emit(f, v0, v1, v2, n.mat);
        }
      }
    }
    // Its shadow: each face turned to the sun, dropped along the sun onto the ground (a hair above
    // it), unlit. The depth buffer keeps it under the object; faces turned away would only overlap.
    if (n.shadow && shadow_mat >= 0 && light_dir[1] > 0.05f) {
      verts.resize(nv);
      const float kd = fdiv(1.0f, light_dir[1]);
      for (size_t i = 0; i < nv; ++i) {
        float w[3];
        n.rot.apply(&o.p[i * 3], w);
        w[0] += n.pos[0];
        w[1] += n.pos[1];
        w[2] += n.pos[2];
        const float k = (w[1] - ground_y) * kd;
        const float g[3] = {w[0] - light_dir[0] * k - cam_pos[0], ground_y + 0.25f - cam_pos[1], w[2] - light_dir[2] * k - cam_pos[2]};
        float q[3];
        cam.apply(g, q);
        V& v = verts[i];
        if (project(q, v)) v.b = 255.0f;
      }
      for (size_t k = 0; k < o.t.size(); k += 3) {
        const V &v0 = verts[tri[k]], &v1 = verts[tri[k + 1]], &v2 = verts[tri[k + 2]];
        if (v0.z < 0 || v1.z < 0 || v2.z < 0) continue;
        // turned to the sun: the face's (world) normal against the sun's direction
        const float *a0 = &o.p[tri[k] * 3], *a1 = &o.p[tri[k + 1] * 3], *a2 = &o.p[tri[k + 2] * 3];
        const float u[3] = {a1[0] - a0[0], a1[1] - a0[1], a1[2] - a0[2]}, w2[3] = {a2[0] - a0[0], a2[1] - a0[1], a2[2] - a0[2]};
        const float nl[3] = {u[1] * w2[2] - u[2] * w2[1], u[2] * w2[0] - u[0] * w2[2], u[0] * w2[1] - u[1] * w2[0]};
        float nw[3];
        n.rot.apply(nl, nw);
        if (nw[0] * light_dir[0] + nw[1] * light_dir[1] + nw[2] * light_dir[2] <= 0) continue;
        emit(f, v0, v1, v2, shadow_mat);
      }
    }
  }
  queued = f.ntris;
}

template <bool kTex>
void Stage::Impl::emit_t(Frame& f, const V& a, const V& b, const V& c, int mat, const float* uv, const Texture* tex) {
  const V* v[3] = {&a, &b, &c};  // sorted by y (ord: where each came from, for a texture's coordinates)
  [[maybe_unused]] int ord[3] = {0, 1, 2};
  if (v[1]->y < v[0]->y) {
    std::swap(v[0], v[1]);
    if constexpr (kTex) std::swap(ord[0], ord[1]);
  }
  if (v[2]->y < v[1]->y) {
    std::swap(v[1], v[2]);
    if constexpr (kTex) std::swap(ord[1], ord[2]);
  }
  if (v[1]->y < v[0]->y) {
    std::swap(v[0], v[1]);
    if constexpr (kTex) std::swap(ord[0], ord[1]);
  }
  const int r0 = std::max(ceil16(v[0]->y - 8), 0), r2 = std::min(ceil16(v[2]->y - 8), rh);
  if (r0 >= r2) return;  // no pixel center between its top and bottom
  const int x0 = v[0]->x, y0 = v[0]->y, x1 = v[1]->x, y1 = v[1]->y, x2 = v[2]->x, y2 = v[2]->y;
  const float ax = (x1 - x0) * (1.0f / kSub), ay = (y1 - y0) * (1.0f / kSub);
  const float bx = (x2 - x0) * (1.0f / kSub), by = (y2 - y0) * (1.0f / kSub);
  const float cross = ax * by - bx * ay;  // > 0: v1 right of the long edge
  if (cross == 0) return;
  Tri& t = f.add();  // built in place
  t.x0 = x0;
  t.y0 = y0;
  t.x1 = x1;
  t.y1 = y1;
  t.r0 = static_cast<int16_t>(r0);
  t.r1 = static_cast<int16_t>(std::clamp(ceil16(y1 - 8), r0, r2));
  t.r2 = static_cast<int16_t>(r2);
  t.s02 = slope(x0, y0, x2, y2);
  t.s01 = slope(x0, y0, x1, y1);
  t.s12 = slope(x1, y1, x2, y2);
  const float z[3] = {v[0]->z, v[1]->z, v[2]->z}, br[3] = {v[0]->b, v[1]->b, v[2]->b};
  const bool flat = br[0] == br[1] && br[1] == br[2];
  t.flags = static_cast<uint8_t>((cross > 0 ? kLongLeft : 0) | (flat ? kFlat : 0));
  // The span loop's start: depth and brightness at the center of (its leftmost column, row r0), so a
  // row's first pixel is a short multiply away (the products stay within the triangle's own ranges).
  const int xb = std::min({x0, x1, x2}) >> 4;
  t.xb = static_cast<int16_t>(xb);
  const float inv = fdiv(1.0f, cross);
  const float dx = xb + 0.5f - x0 * (1.0f / kSub), dy = r0 + 0.5f - y0 * (1.0f / kSub);
  {
    const float fzx = ((z[1] - z[0]) * by - (z[2] - z[0]) * ay) * inv, fzy = ((z[2] - z[0]) * ax - (z[1] - z[0]) * bx) * inv;
    t.dzx = round_fx(fzx * 256.0f);
    t.dzy = round_fx(fzy * 256.0f);
    t.zs = round_fx((z[0] + fzx * dx + fzy * dy) * 256.0f);
  }
  if (flat) {
    t.dbx = t.dby = 0;
    t.bs = round_fx(br[0] * 65536.0f);
  } else {
    const float fbx = ((br[1] - br[0]) * by - (br[2] - br[0]) * ay) * inv, fby = ((br[2] - br[0]) * ax - (br[1] - br[0]) * bx) * inv;
    t.dbx = round_fx(fbx * 65536.0f);
    t.dby = round_fx(fby * 65536.0f);
    t.bs = round_fx((br[0] + fbx * dx + fby * dy) * 65536.0f);
  }
  t.mat = static_cast<uint8_t>(mat);
  t.tex = 0;
  if constexpr (kTex) {
    if (f.texrecs.size() < 0xFFFF) {
      // u, v moved by whole repeats to near 0 (the texture repeats: the same texels, smaller numbers)
      float u[3], w[3];
      for (int k = 0; k < 3; ++k) {
        u[k] = uv[ord[k] * 2];
        w[k] = uv[ord[k] * 2 + 1];
      }
      const float tw = static_cast<float>(1 << tex->wbits), th = static_cast<float>(1 << tex->hbits);
      const float su = std::floor(std::min({u[0], u[1], u[2]}) / tw) * tw, sv = std::floor(std::min({w[0], w[1], w[2]}) / th) * th;
      for (int k = 0; k < 3; ++k) {
        u[k] -= su;
        w[k] -= sv;
      }
      const float fux = ((u[1] - u[0]) * by - (u[2] - u[0]) * ay) * inv, fuy = ((u[2] - u[0]) * ax - (u[1] - u[0]) * bx) * inv;
      const float fvx = ((w[1] - w[0]) * by - (w[2] - w[0]) * ay) * inv, fvy = ((w[2] - w[0]) * ax - (w[1] - w[0]) * bx) * inv;
      auto wrap = [](float x) {  // 16.16, kept modulo 2^32 (only the low bits address texels)
        return static_cast<uint32_t>(static_cast<int64_t>(x >= 0 ? x + 0.5f : x - 0.5f));
      };
      TexRec r;
      const float vk = 65536.0f * tw;  // v's 16.16, times the width
      r.dux = round_fx(fux * 65536.0f);
      r.duy = round_fx(fuy * 65536.0f);
      r.dvx = round_fx(fvx * vk);
      r.dvy = round_fx(fvy * vk);
      r.us = wrap((u[0] + fux * dx + fuy * dy) * 65536.0f);
      r.vs = wrap((w[0] + fvx * dx + fvy * dy) * vk);
      r.texels = tex->texels.data();
      r.um = static_cast<uint16_t>((1 << tex->wbits) - 1);
      r.vm = static_cast<uint16_t>(((1 << tex->hbits) - 1) << tex->wbits);
      t.tex = static_cast<uint16_t>(f.texrecs.size());
      t.flags |= kTextured;
      f.texrecs.push_back(r);
    }
  }
  const int i = f.ntris - 1;
  for (int k = r0 / kPiece; k <= (r2 - 1) / kPiece; ++k) f.bins[k].push_back(i);
}

void Stage::prepare() {
  Impl& p = *p_;
  if (p.want_scale != p.scale && !p.jet_raster && p.jet_objects.empty()) {
    p.scale = p.want_scale;
    p.rw = Impl::rw_of(p.out_w, p.scale);
    p.rh = kH / p.scale;
  }
  Frame& f = p.frames[p.front ^ 1];
  if (f.scale != p.scale) f.resize(p.scale, p.rw, p.rh);
  for (auto& b : f.bins) b.clear();
  const auto t0 = std::chrono::steady_clock::now();
  if (f.sky_version != p.scene_version) {  // the background, top to bottom (the panel's byte order)
    for (int y = 0; y < p.rh; ++y) {
      const float k = static_cast<float>(y) / std::max(1, p.rh - 1);
      auto ch = [&](int sh) { return static_cast<int>((p.sky_top >> sh & 255) * (1 - k) + (p.sky_bottom >> sh & 255) * k + 0.5f); };
      f.sky[y] = panel(rgb565(ch(16), ch(8), ch(0)));
    }
    f.sky_version = p.scene_version;
  }
  if (p.jet_raster) p.prepare_jet(f);
  else p.prepare_ours(f);
  p.jet_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
}

void Stage::publish() { p_->front ^= 1; }

int Stage::band_rows() const { return kPiece * 2; }  // a piece at half size (two at full)
int Stage::work_cells() const {  // depth, and a color piece at half size: whichever scale needs more
  return kAlignCells + std::max(Impl::rw_of(p_->out_w, 1) * kPiece, Impl::rw_of(p_->out_w, 2) * kPiece * 2);
}

namespace {

#ifdef HD_PIE
// stage_pie.S: the span loop 8 pixels at a time, and the pieces' fills
extern "C" void stage_pie_span(void* s);
extern "C" void stage_pie_fill(void* p, int n16, uint32_t v);
struct alignas(16) PieSpan {
  int32_t z[8];         // the first group's depths
  int32_t dz8;          // 8 x depth per pixel
  uint32_t bias = 0x80008000u;
  uint32_t color;      // a flat triangle's, both halves
  int32_t flat;
  uint16_t gather[8];
  uint16_t *zr, *cr;    // the rows at the first group (aligned to 16 bytes)
  int32_t n8;           // groups
  int32_t b, dbx;
  const uint16_t* lut;
};
static_assert(offsetof(PieSpan, dz8) == 32 && offsetof(PieSpan, gather) == 48 && offsetof(PieSpan, zr) == 64 &&
              offsetof(PieSpan, n8) == 72 && offsetof(PieSpan, lut) == 84, "stage_pie.S's offsets");
int g_pie_min = 24;  // a run this long goes to the vector loop (shorter: the setup costs more than it saves)
#endif
bool g_pie = true;  // the vector span loop (off: the scalar loop everywhere, for selftest())

// n words of v from w (4-byte aligned): 16 bytes a store on the S3
STAGE_INLINE void fill32(uint32_t* w, int n, uint32_t v) {
#ifdef HD_PIE
  if (n >= 8) {
    while (reinterpret_cast<uintptr_t>(w) & 15) {
      *w++ = v;
      --n;
    }
    stage_pie_fill(w, n >> 2, v);
    w += n & ~3;
    n &= 3;
  }
#endif
  while (n-- > 0) *w++ = v;
}

// A textured triangle's rows: as draw_tri's walk, each pixel's texel looked up at its (u, v) (16.16,
// wrapped: the texture repeats) and its color at its brightness level (the table's rows of 16).
STAGE_HOT void draw_tex(const Tri& t, const TexRec& x, const uint16_t* lut, int ra, int rb, int c0, int c1, uint16_t* depth, uint16_t* col,
                        int rw) {
  const int top = std::max<int>(t.r0, ra), end = std::min<int>(t.r2, rb);
  if (top >= end) return;
  struct Edge {
    uint32_t x;
    int32_t k;
  };
  auto edge = [](int xa, int ya, int32_t s, int y) {  // (draw_tri's)
    const int64_t e = (static_cast<int64_t>(xa) << 16) + static_cast<int64_t>(16 * y + 8 - ya) * s;
    return Edge{static_cast<uint32_t>(e >> 4), static_cast<int32_t>(((e & 15) + (1 << 19) - 1) >> 4)};
  };
  Edge el = edge(t.x0, t.y0, t.s02, top), es;
  const uint32_t sl = static_cast<uint32_t>(t.s02);
  uint32_t ss;
  if (top < t.r1) {
    es = edge(t.x0, t.y0, t.s01, top);
    ss = static_cast<uint32_t>(t.s01);
  } else {
    es = edge(t.x1, t.y1, t.s12, top);
    ss = static_cast<uint32_t>(t.s12);
  }
  const bool long_left = t.flags & kLongLeft;
  const int dr = top - t.r0;
  int32_t zrow = t.zs + dr * t.dzy, brow = t.bs + dr * t.dby;
  uint32_t urow = x.us + static_cast<uint32_t>(dr * x.duy), vrow = x.vs + static_cast<uint32_t>(dr * x.dvy);
  const int32_t dzx = t.dzx, dbx = t.dbx;
  const uint32_t dux = static_cast<uint32_t>(x.dux), dvx = static_cast<uint32_t>(x.dvx);
  const uint32_t um = x.um, vm = x.vm;
  const uint8_t* tx = x.texels;
  const bool flat = t.flags & kFlat;
  const uint16_t* row = lut + (t.bs >> 19) * 16;  // a flat triangle's: its one brightness level
  for (int y = top; y < end; ++y, el.x += sl, es.x += ss, zrow += t.dzy, brow += t.dby, urow += static_cast<uint32_t>(x.duy), vrow += static_cast<uint32_t>(x.dvy)) {
    if (y == t.r1 && y != top) {
      es = edge(t.x1, t.y1, t.s12, y);
      ss = static_cast<uint32_t>(t.s12);
    }
    const int32_t pl = static_cast<int32_t>(el.x + static_cast<uint32_t>(el.k)) >> 16;
    const int32_t ps = static_cast<int32_t>(es.x + static_cast<uint32_t>(es.k)) >> 16;
    int32_t x0 = long_left ? pl : ps, x1 = long_left ? ps : pl;
    x0 = std::max(x0, static_cast<int32_t>(c0));
    x1 = std::min(x1, static_cast<int32_t>(c1));
    if (x0 >= x1) continue;
    const int32_t o = x0 - t.xb;
    int32_t z = zrow + o * dzx;
    uint32_t u = urow + static_cast<uint32_t>(o) * dux, v = vrow + static_cast<uint32_t>(o) * dvx;
    uint16_t* __restrict zr = depth + (y - ra) * rw + x0;
    uint16_t* __restrict cr = col + (y - ra) * rw + x0;
    const int n = x1 - x0;
    if (flat) {  // one level: the row's colors by texel
      for (int i = 0; i < n; ++i, z += dzx, u += dux, v += dvx) {
        const uint32_t zz = static_cast<uint32_t>(z) >> 8;
        if (zz < zr[i]) {
          zr[i] = static_cast<uint16_t>(zz);
          cr[i] = row[tx[(v >> 16 & vm) | (u >> 16 & um)]];
        }
      }
    } else {
      int32_t b = brow + o * dbx;
      for (int i = 0; i < n; ++i, z += dzx, b += dbx, u += dux, v += dvx) {
        const uint32_t zz = static_cast<uint32_t>(z) >> 8;
        if (zz < zr[i]) {
          zr[i] = static_cast<uint16_t>(zz);
          cr[i] = lut[(b >> 19) * 16 + tx[(v >> 16 & vm) | (u >> 16 & um)]];
        }
      }
    }
  }
}

// One triangle's rows [ra, rb) and columns [c0, c1) of a piece: depth and color rows of width rw,
// row ra at index 0. Per pixel: a depth test, a table lookup (a flat triangle: one color), two adds;
// on the S3 a long run 8 pixels at a time (stage_pie.S). depth and col: the same alignment (band()).
STAGE_HOT void draw_tri(const Tri& t, const uint16_t* lut, int ra, int rb, int c0, int c1, uint16_t* depth, uint16_t* col, int rw) {
  const int top = std::max<int>(t.r0, ra), end = std::min<int>(t.r2, rb);
  if (top >= end) return;
  // Each edge walked in 32 bits. Its exact x at a row's center, E (1/16 px, 16 fraction bits: 64-bit),
  // is X * 16 + r with r the same on every row (a row adds 16 x the slope), so X steps by the slope,
  // exactly, and the first pixel whose center (16 px + 8) is at or right of the edge, E + 2^19 - 1 >> 20,
  // is X + k >> 16 with k = r + 2^19 - 1 >> 4. (An edge gives the same x on a row whichever triangle
  // walks it; on its rows |x| stays under 2^15 px, so X fits.)
  struct Edge {
    uint32_t x;
    int32_t k;
  };
  auto edge = [](int xa, int ya, int32_t s, int y) {
    const int64_t e = (static_cast<int64_t>(xa) << 16) + static_cast<int64_t>(16 * y + 8 - ya) * s;
    return Edge{static_cast<uint32_t>(e >> 4), static_cast<int32_t>(((e & 15) + (1 << 19) - 1) >> 4)};
  };
  Edge el = edge(t.x0, t.y0, t.s02, top), es;
  const uint32_t sl = static_cast<uint32_t>(t.s02);
  uint32_t ss;
  if (top < t.r1) {
    es = edge(t.x0, t.y0, t.s01, top);
    ss = static_cast<uint32_t>(t.s01);
  } else {
    es = edge(t.x1, t.y1, t.s12, top);
    ss = static_cast<uint32_t>(t.s12);
  }
  const bool long_left = t.flags & kLongLeft, flat = t.flags & kFlat;
  int32_t zrow = t.zs + (top - t.r0) * t.dzy, brow = t.bs + (top - t.r0) * t.dby;
  const int32_t dzx = t.dzx, dbx = t.dbx;
  const uint16_t fc = flat ? lut[t.bs >> 16] : 0;  // a flat triangle's color
#ifdef HD_PIE
  PieSpan pie;
  pie.flat = flat;
  pie.color = fc * 0x10001u;
  pie.dz8 = 8 * dzx;
  pie.dbx = dbx;
  pie.lut = lut;
#endif
  for (int y = top; y < end; ++y, el.x += sl, es.x += ss, zrow += t.dzy, brow += t.dby) {
    if (y == t.r1 && y != top) {  // the short side turns the corner at v1
      es = edge(t.x1, t.y1, t.s12, y);
      ss = static_cast<uint32_t>(t.s12);
    }
    const int32_t pl = static_cast<int32_t>(el.x + static_cast<uint32_t>(el.k)) >> 16;
    const int32_t ps = static_cast<int32_t>(es.x + static_cast<uint32_t>(es.k)) >> 16;
    int32_t x0 = long_left ? pl : ps, x1 = long_left ? ps : pl;
    x0 = std::max(x0, static_cast<int32_t>(c0));
    x1 = std::min(x1, static_cast<int32_t>(c1));
    if (x0 >= x1) continue;
    int32_t z = zrow + (x0 - t.xb) * dzx, b = brow + (x0 - t.xb) * dbx;
    uint16_t* zr = depth + (y - ra) * rw;
    uint16_t* cr = col + (y - ra) * rw;
    int32_t x = x0;
#ifdef HD_PIE
    if (g_pie && x1 - x0 >= g_pie_min) {
      // one by one to where the rows are aligned to 16 bytes, then 8 at a time
      const int32_t xa = x0 + static_cast<int32_t>(((16 - (reinterpret_cast<uintptr_t>(cr + x0) & 15)) & 15) >> 1);
      if (flat)
        for (; x < xa; ++x, z += dzx) {
          const uint32_t zz = static_cast<uint32_t>(z) >> 8;
          if (zz < zr[x]) {
            zr[x] = static_cast<uint16_t>(zz);
            cr[x] = fc;
          }
        }
      else
        for (; x < xa; ++x, z += dzx, b += dbx) {
          const uint32_t zz = static_cast<uint32_t>(z) >> 8;
          if (zz < zr[x]) {
            zr[x] = static_cast<uint16_t>(zz);
            cr[x] = lut[b >> 16];
          }
        }
      const int32_t n8 = (x1 - x) >> 3;
      for (int i = 0; i < 8; ++i) pie.z[i] = z + i * dzx;
      pie.zr = zr + x;
      pie.cr = cr + x;
      pie.n8 = n8;
      pie.b = b;
      stage_pie_span(&pie);
      x += 8 * n8;
      z += 8 * n8 * dzx;
      b += 8 * n8 * dbx;
    }
#endif
    if (flat) {
      for (; x < x1; ++x, z += dzx) {
        const uint32_t zz = static_cast<uint32_t>(z) >> 8;
        if (zz < zr[x]) {
          zr[x] = static_cast<uint16_t>(zz);
          cr[x] = fc;
        }
      }
    } else {
      for (; x < x1; ++x, z += dzx, b += dbx) {
        const uint32_t zz = static_cast<uint32_t>(z) >> 8;
        if (zz < zr[x]) {
          zr[x] = static_cast<uint16_t>(zz);
          cr[x] = lut[b >> 16];
        }
      }
    }
  }
}

}  // namespace

int Stage::band(int y0, int y1, uint16_t* out, uint16_t* work, int x0, int x1) {
  Impl& p = *p_;
  Frame& f = p.frames[p.front];
  const int rw = f.rw, sc = f.scale, W = p.out_w;
  if (x1 < 0) x1 = W;
  const int c0 = x0 / sc, c1 = std::min((x1 + 2 * sc - 1) / (2 * sc) * 2, rw);  // rendered columns [c0, c1), even
  const bool ours = !p.jet_raster;       // ours draws in the panel's byte order; Jet's in RGB565
  const uint16_t* luts[kMaxMaterials];  // each table at its brightness 0 (a textured one's row 0)
  for (size_t i = 0; i < f.luts.size(); ++i) luts[i] = f.luts[i] ? f.luts[i].get() + (p.mats[i].tex >= 0 ? 16 : kLutPad) : nullptr;
  const TexRec* texrecs = f.texrecs.data();
  int drawn = 0;
  for (int a = y0 / sc; a < y1 / sc; a += kPiece) {  // rendered rows [a, a + kPiece)
    // The piece's color: straight into out at full size, else a piece of work to be doubled.
    uint16_t* col = sc == 1 ? out + (a - y0) * W : work + kAlignCells + rw * kPiece;
    // Its depth: where its rows have the color rows' alignment (the vector span loop's 16 bytes).
    uint16_t* depth = work + ((reinterpret_cast<uintptr_t>(col) - reinterpret_cast<uintptr_t>(work)) & 15) / 2;
    for (int r = 0; r < kPiece; ++r)  // the sky, two pixels a word
      fill32(reinterpret_cast<uint32_t*>(col + r * rw) + c0 / 2, (c1 - c0) / 2, (ours ? f.sky[a + r] : panel(f.sky[a + r])) * 0x10001u);
    const auto& bin = f.bins[a / kPiece];
    if (!bin.empty()) {
      for (int r = 0; r < kPiece; ++r) fill32(reinterpret_cast<uint32_t*>(depth + r * rw) + c0 / 2, (c1 - c0) / 2, 0xFFFFFFFFu);
      if (ours) {
        for (const int32_t i : bin) {
          const Tri& t = f.tris[i];
          if (t.flags & kTextured) draw_tex(t, texrecs[t.tex], luts[t.mat], a, a + kPiece, c0, c1, depth, col, rw);
          else draw_tri(t, luts[t.mat], a, a + kPiece, c0, c1, depth, col, rw);
        }
        drawn += static_cast<int>(bin.size());
      } else {
        drawn += f.scene.rasterizeListInto(a, a + kPiece, col - a * rw, depth - a * rw, bin.data(), static_cast<int>(bin.size()), c0, c1);
      }
    }
    if (sc == 1) {
      if (!ours)  // to the panel's byte order (big-endian), two pixels a word
        for (int r = 0; r < kPiece; ++r) {
          auto* w = reinterpret_cast<uint32_t*>(col + r * rw);
          for (int x = c0 / 2; x < c1 / 2; ++x) w[x] = (w[x] & 0x00FF00FFu) << 8 | (w[x] >> 8 & 0x00FF00FFu);
        }
    } else {  // each pixel 2x2 (byte-swapped on the way for Jet's): two source pixels a word in, four out
      const int xe = x1 / 4;  // whole words out; then, at an output edge 2 pixels past one, half a word
      for (int r = 0; r < kPiece; ++r) {
        const auto* src = reinterpret_cast<const uint32_t*>(col + r * rw);
        auto* d0 = reinterpret_cast<uint32_t*>(out + (2 * (a + r) - y0) * W);
        uint32_t* d1 = d0 + W / 2;
        int x = c0 / 2;
        for (; x < xe; ++x) {
          const uint32_t q = ours ? src[x] : (src[x] & 0x00FF00FFu) << 8 | (src[x] >> 8 & 0x00FF00FFu);
          const uint32_t lo = (q & 0xFFFFu) * 0x10001u, hi = (q >> 16) * 0x10001u;
          d0[2 * x] = lo;
          d0[2 * x + 1] = hi;
          d1[2 * x] = lo;
          d1[2 * x + 1] = hi;
        }
        if (x1 & 2) {
          const uint32_t q = ours ? src[x] : (src[x] & 0x00FF00FFu) << 8 | (src[x] >> 8 & 0x00FF00FFu);
          d0[2 * x] = d1[2 * x] = (q & 0xFFFFu) * 0x10001u;
        }
      }
    }
  }
  return drawn;
}

void Stage::use_jet_raster(bool on) { p_->jet_raster = on; }
int Stage::scale() const { return p_->scale; }
void Stage::set_scale(int s) { p_->want_scale = s == 2 ? 2 : 1; }
int Stage::wanted_scale() const { return p_->want_scale; }
void Stage::set_auto_res(bool on) { p_->auto_res = on; }
bool Stage::auto_res() const { return p_->auto_res; }
int Stage::width() const { return p_->out_w; }
int64_t Stage::prepare_jet_us() const { return p_->jet_us; }
int Stage::triangles() const { return p_->tris; }  // in the scene's shapes
int Stage::queued() const { return p_->queued; }

// ---- the vector loop's check ----

std::string Stage::selftest() {
#ifndef HD_PIE
  return "no vector loops";
#else
  std::string report;
  int bad = 0;
  {  // fdiv against the toolchain's a / b, bit for bit
    uint32_t r = 987654321u;
    auto rnd = [&]() { return r = r * 1664525u + 1013904223u; };
    for (int i = 0; i < 20000; ++i) {
      uint32_t ua = rnd(), ub = rnd();
      if (i % 4 == 0) ua = (ua & 0x807FFFFFu) | ((rnd() % 60 + 97) << 23);  // ordinary magnitudes too
      if (i % 4 == 1) ub = (ub & 0x807FFFFFu) | ((rnd() % 60 + 97) << 23);
      float a, b;
      std::memcpy(&a, &ua, 4);
      std::memcpy(&b, &ub, 4);
      if (!std::isfinite(a) || !std::isfinite(b) || b == 0) continue;
      const volatile float want = a / b;
      const float got = fdiv(a, b);
      uint32_t w, g;
      const float wv = want;
      std::memcpy(&w, &wv, 4);
      std::memcpy(&g, &got, 4);
      if (w != g && !(std::isnan(wv) && std::isnan(got)) && bad++ == 0) {
        char m[120];
        snprintf(m, sizeof m, "fdiv %08x / %08x: %08x, a / b %08x", static_cast<unsigned>(ua), static_cast<unsigned>(ub), static_cast<unsigned>(g),
                 static_cast<unsigned>(w));
        report = m;
      }
    }
  }
  constexpr int kModes = 5, kMin[kModes] = {16, 20, 24, 32, 0};  // the vector loop from runs this long; 0: off
  double us[kModes] = {};
  int frames = 0;
  std::string per;
  // Scenes whose spans are long (shapes near the camera, flat and lit, a sliver or two) and short (rings)
  for (int scene = 0; scene < 5; ++scene) {  // (4: as 1, switched to half size: the glass's width, 410, halved)
    const int sc = scene == 3 ? 2 : 1, w = sc == 2 ? 384 : 410;
    Stage st(sc, w, sc == 2 ? 192.0f : 12 + kW / 2.0f);
    if (scene == 0) {
      st.build_rings(32);
    } else {
      const float phi = 1.618034f;
      std::vector<float> ico = {-1, phi, 0, 1, phi, 0, -1, -phi, 0, 1, -phi, 0, 0, -1, phi, 0, 1, phi,
                                0, -1, -phi, 0, 1, -phi, phi, 0, -1, phi, 0, 1, -phi, 0, -1, -phi, 0, 1};
      for (float& v : ico) v *= 60;
      const std::vector<int> faces = {0, 11, 5, 0, 5, 1, 0, 1, 7, 0, 7, 10, 0, 10, 11, 1, 5, 9, 5, 11, 4, 11, 10, 2, 10, 7, 6, 7, 1, 8,
                                      3, 9, 4, 3, 4, 2, 3, 2, 6, 3, 6, 8, 3, 8, 9, 4, 9, 5, 2, 4, 11, 6, 2, 10, 8, 6, 7, 9, 8, 1};
      const int shapes[5] = {st.box(140, 120, 160), st.sphere(90, 24), st.cylinder(60, 200, 20), st.mesh(ico, faces, false), st.box(900, 4, 900)};
      const uint32_t cols[5] = {0xE04040, 0x40A0E0, 0x40C060, 0xF0F0F0, 0x506070};
      for (int i = 0; i < 5; ++i) {
        const int o = st.add(shapes[i], cols[i], i * 50, i == 2);
        st.place(o, i == 4 ? 0 : -150.0f + 100 * i, i == 4 ? -110.0f : 20.0f * (i % 2), i == 4 ? 300.0f : 60.0f * i - 40, 20 + 13 * scene, 30 * scene + 25 * i, 7 * i);
      }
      {  // textures on the box and the floor (a checker, 8 x 8; bricks, 16 x 4)
        std::vector<uint8_t> chk(64), brk(64);
        for (int i = 0; i < 64; ++i) {
          chk[i] = static_cast<uint8_t>(((i & 7) + (i >> 3)) % 2 ? 8 : 7);
          brk[i] = static_cast<uint8_t>((i >> 4) == 3 || (i & 15) == 7 ? 5 : 4);
        }
        uint32_t pal[16];
        for (int k = 0; k < 16; ++k) pal[k] = 0x101010u * static_cast<uint32_t>(k) + 0x203040u;
        const int t0 = st.texture(8, 8, chk, pal), t1 = st.texture(16, 4, brk, pal);
        st.skin(0, t0, 2, 2);
        st.skin(4, t1, 20, 60);
      }
      st.view(10.0f * scene, 40, scene == 2 ? -260.0f : -380.0f, 4, 6, 70);
      st.ground(-108, 0x202830);
      st.shadow(0, true);
      st.shadow(1, true);
    }
    if (scene == 4) st.set_scale(2);
    st.step(1.3f, 0, 0);
    st.prepare();
    st.publish();
    const int W = st.width(), rows = sc == 1 && scene != 4 ? kPiece : st.band_rows();  // (full size: a piece at a time, as fits in internal RAM)
    double u0[kModes];
    std::copy(us, us + kModes, u0);
    // band and work cells in internal RAM, as the sender's
    const size_t nb = static_cast<size_t>(W) * rows + 8, nw = static_cast<size_t>(st.work_cells()) + 8;
#if defined(ESP_PLATFORM)
    auto* band = static_cast<uint16_t*>(heap_caps_malloc((nb + nw) * 2, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
#else
    uint16_t* band = nullptr;
#endif
    const bool timed = band != nullptr;  // (else checked, not timed: PSRAM's time isn't the sender's)
    if (!band) band = static_cast<uint16_t*>(malloc((nb + nw) * 2));
    if (!band) return "no memory for the test's band";
    uint16_t* work = band + nb;
    std::vector<uint16_t> pic[kModes];
    for (int mode = 0; mode < kModes; ++mode) {
      g_pie = kMin[mode] > 0;
      g_pie_min = kMin[mode];
      pic[mode].assign(static_cast<size_t>(W) * kH, 0);
      for (int shift = 0; shift < 4; ++shift) {  // the band buffer at each 16-byte phase (4-byte steps)
        uint16_t* out = band + 2 * shift;
        for (int y = 0; y < kH; y += rows) {
          const auto t0 = std::chrono::steady_clock::now();
          st.band(y, y + rows, out, work + 2 * (shift & 1), 0, W / 2 / 4 * 4);
          st.band(y, y + rows, out, work + 2 * (shift & 1), W / 2 / 4 * 4, -1);
          if (timed) us[mode] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count();
          if (shift == 0) std::memcpy(&pic[mode][static_cast<size_t>(y) * W], out, static_cast<size_t>(W) * rows * 2);
          else if (std::memcmp(&pic[mode][static_cast<size_t>(y) * W], out, static_cast<size_t>(W) * rows * 2) != 0 && bad++ == 0)
            report = "scene " + std::to_string(scene) + ": the band buffer's alignment changed the picture (mode " + std::to_string(mode) + ")";
        }
      }
    }
    free(band);
    frames += timed ? 4 : 0;
    if (timed) {
      per += " scene " + std::to_string(scene) + ":";
      for (int m = 0; m < kModes; ++m) {
        char b[16];
        snprintf(b, sizeof b, " %.2f", (us[m] - u0[m]) / 4000);
        per += b;
      }
    }
    g_pie = true;
    g_pie_min = 24;
    for (int m = 0; m + 1 < kModes; ++m)
      for (size_t i = 0; i < pic[m].size(); ++i)
        if (pic[m][i] != pic[kModes - 1][i] && bad++ == 0)
          report = "scene " + std::to_string(scene) + ": pixel (" + std::to_string(i % W) + ", " + std::to_string(i / W) + ") vector " +
                   std::to_string(pic[m][i]) + ", scalar " + std::to_string(pic[kModes - 1][i]);
  }
  char line[160];
  snprintf(line, sizeof line, "%s; drawing %.2f ms a frame with the vector loop, %.2f without (%d frames timed; per scene, ms, runs from 16 20 24 32 px, scalar)",
           bad ? "MISMATCHES" : "bit for bit", frames ? us[2] / frames / 1000 : 0.0, frames ? us[kModes - 1] / frames / 1000 : 0.0, frames);
  per = std::string(line) + ";" + per;
  {  // preparing: the rings of 48 segments (4608 triangles) at 192x192, whole and without parts
    Stage st(2);
    st.build_rings(48);
    double t[3] = {};
    for (int skip = 0; skip < 3; ++skip) {
      g_prep_skip = skip;
      for (int i = 0; i < 20; ++i) {
        st.step(1.3f + i * 0.05f, 0, 0);
        const auto t0 = std::chrono::steady_clock::now();
        st.prepare();
        t[skip] += std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / 20;
      }
    }
    g_prep_skip = 0;
    char b[300];
    snprintf(b, sizeof b, "; prepare (4608 tris): %.2f ms; set-up %.2f, culling and vertices %.2f, culling alone %.2f", t[0] / 1000,
             (t[0] - t[1]) / 1000, t[1] / 1000, t[2] / 1000);
    per += b;
  }
  return report.empty() ? per : per + ": " + std::to_string(bad) + ", first: " + report;
#endif
}

}  // namespace carts

