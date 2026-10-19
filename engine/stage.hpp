// Carts' 3D stage: a lit, depth-buffered scene drawn band by band into the caller's band buffers in
// the panel's byte order, at 384x384 (or 192x192 doubled). prepare() runs once a frame (each
// vertex transformed, projected and lit; each triangle set up and binned) into a back frame while
// band() draws the front one's rows, from any core, each caller with its own work cells. No
// framebuffer: a band is drawn just before it goes to the panel. Lighting follows Jet's model
// (carts/jet, also the comparison path's pipeline and rasteriser).
// Portable (no ESP-IDF): the host's stagerun and cartrun render it too.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace carts {

class Stage {
 public:
  static constexpr int kW = 384, kH = 384;  // the cart window (the 128x128 picture's 3x): fov spans its width
  static constexpr int kPiece = 16;         // rendered rows drawn against one depth buffer
  // What a scene may hold (memory; a frame draws ~1500 front-facing triangles at 60 fps, ~2000 at 50).
  static constexpr int kMaxMeshes = 64, kMaxNodes = 128, kMaxMaterials = 64;
  static constexpr int kMaxVertices = 16384, kMaxTriangles = 16384;  // all shapes together
  static constexpr int kMaxTextures = 16;

  // scale 1: drawn at full size; 2: at half, each pixel sent as 2x2 (a quarter of the drawing).
  // width: what goes out, kH rows of it (wider than the window: the scene runs past it to the
  // glass's edges); centre_x: where the window's centre is in it. A multiple of 4 at scale 2.
  explicit Stage(int scale = 1, int width = kW, float centre_x = kW / 2.0f);
  ~Stage();

  // ---- the scene (the Lua API's, docs/api.md) ----
  // Shapes, in world units (y up). Each returns its id, or -1 and error() says why.
  int box(float w, float h, float d);
  int sphere(float r, int segs);
  int torus(float big_r, float small_r, int segs);  // a ring around the z axis
  int cylinder(float r, float h, int segs);         // along y
  int cone(float r, float h, int segs);             // along y, the point up
  // Vertices (x, y, z, ...) and triangles (indices into them, from 0), each face turned outward from
  // the shape's centre (meant for convex shapes, as dice are). smooth: normals averaged at shared
  // vertices; else each face flat.
  // uv: a texture's coordinates (u, v, ...), a pair a vertex, 0..1 across it (it repeats past).
  int mesh(const std::vector<float>& xyz, const std::vector<int>& tris, bool smooth, const std::vector<float>* uv = nullptr);
  // An object: a shape with a colour (0xRRGGBB), a shine (specular, 0..255) and unlit (its colour
  // as is). Returns its id, or -1.
  int add(int mesh, uint32_t rgb, int shine, bool unlit);
  bool place(int obj, float x, float y, float z, float rx, float ry, float rz);  // Euler degrees: x, then y, then z
  bool orient(int obj, const float m[9]);                                       // a rotation matrix, rows
  bool hide(int obj, bool hidden);
  bool recolour(int obj, uint32_t rgb, int shine, bool unlit);
  // A texture: w x h texels (4 to 64 each way, powers of two), each an index into palette (16
  // colours, 0xRRGGBB). Returns its id, or -1. Shapes carry (u, v) coordinates: a box's faces each
  // the whole texture, a sphere's, cylinder's and cone's around their side (caps across), a torus's
  // around and through its tube.
  int texture(int w, int h, const std::vector<uint8_t>& texels, const uint32_t palette[16]);
  // An object in a texture (tex -1: its colour again), repeated ru x rv times across the shape, lit
  // as its shine and unlit say. Mapped affinely across each triangle (the PS1's way: it bends on
  // big triangles near the camera; smaller ones bend less).
  bool skin(int obj, int tex, float ru, float rv);
  // The camera at (x, y, z), looking along +z at yaw 0 (yaw 90: along +x; pitch 90: up); fov degrees.
  void view(float x, float y, float z, float yaw, float pitch, float fov);
  // The sun: azimuth and elevation in degrees (Jet's), intensity 0..255, and the ambient colour.
  void light(float azimuth, float elevation, int intensity, uint32_t ambient_rgb);
  void sky(uint32_t top_rgb, uint32_t bottom_rgb);  // the background, top to bottom
  // Shadows: objects with shadow() on cast them along the sun onto the plane y = height, in
  // `rgb` (the ground's colour in shadow).
  void ground(float height, uint32_t rgb);
  bool shadow(int obj, bool on);
  // A world point on the output (0..width(), 0..kH; its depth along the view). False: behind the camera.
  bool project(float x, float y, float z, float* sx, float* sy, float* depth) const;
  const std::string& error() const;
  int objects() const;  // in the scene

  // The test scene: two interlocked, smooth-lit rings. `segs`: segments around each ring (its
  // tube gets half), so 2 * segs * segs triangles in all. step(): seconds since the start, and a
  // drag (screen pixels since the finger came down).
  void build_rings(int segs);
  void step(float t, float drag_x, float drag_y);

  // ---- drawing ----
  // Once a frame, into the back frame; band() draws the front one meanwhile (core 1 prepares
  // while core 0 draws). publish(): the back frame becomes the front, when no band() is drawing.
  void prepare();
  void publish();
  // Output rows [y0, y1) (multiples of band_rows()), columns [x0, x1) (even; at scale 2 multiples
  // of 4; x1 < 0: to width()), into out ((y1 - y0) * width() pixels, the panel's byte order). work:
  // work_cells() cells. Two cores can draw a band's two halves at once, sharing out and work (they
  // touch different columns), or different bands with work each. Returns the triangles drawn (once
  // for each piece they cross).
  int band(int y0, int y1, uint16_t* out, uint16_t* work, int x0 = 0, int x1 = -1);
  int width() const;
  int band_rows() const;
  int work_cells() const;

  // The test scene through Jet's own pipeline and rasteriser instead (for comparison).
  void use_jet_raster(bool on);

  // The drawing's scale (1: full size, 2: half), from the next prepare() (the frame being drawn
  // keeps its own): a cart's res(). auto_res: the port picks the scale by the frames' time.
  int scale() const;
  void set_scale(int s);
  int wanted_scale() const;
  void set_auto_res(bool on);
  bool auto_res() const;
  // The vector span loop (the S3's) against the scalar one: test scenes drawn both ways, compared
  // pixel for pixel and timed. Returns the verdict.
  static std::string selftest();
  int64_t prepare_jet_us() const;  // the last prepare()'s time
  int triangles() const;  // in the scene's shapes
  int queued() const;     // after culling, the last prepared frame

 private:
  struct Impl;
  std::unique_ptr<Impl> p_;
};

}  // namespace carts
