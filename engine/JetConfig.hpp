// Jet's build options (carts/jet: the vendored renderer; options as in Jet's JetConfig.example.hpp)
// for carts' 3D: full-resolution RGB565 drawn band by band into the panel's band buffers
// (stage.hpp), from both cores. Not drawn whole: no framebuffer, no field buffers, no
// buffered post effects.
#pragma once

#define JET_MESH_INSTANCING 0
#define RENDER_TILE_BUFFER 0
#define TILE_WIDTH 32
#define TILE_HEIGHT 32

// A depth buffer a band tall (in internal RAM, one per core), interpolated per pixel: meshes that
// pass through each other cut cleanly, which the painter's order can't do.
#define Z_BUFFERING 1
#define FAST_Z 1
#define JET_PERSPECTIVE_DEPTH 0
#define LAZY_Z 0
#define JET_DEPTH_SORT_OPAQUE_FRONT_TO_BACK 1  // near first: hidden pixels fail the depth test unshaded
#define SORT_TRIANGLES 1
#define JET_SORT_DEPTH_BUCKETS 64
#define SORT_SCENE_OBJECTS 0
#define SORT_SCENE_REVERSE 0

#define SCREEN_DOOR_ALPHA 0
#define NOISE_ALPHA 0
#define DEPTH_ALPHA_BLEND 0
#define SKIP_ZERO_AREA_TRIANGLES 1

#define TEXTURE_MAPPING 0
#define PERSPECTIVE_CORRECT_TEXTURES 0
#define JET_HIGH_PRECISION_UVS 0
#define BILINEAR_FILTER 0

#define LIGHTING 1
#define Z_BRIGHTNESS 0
#define FLOAT_CAMERA_ANGLES 1
#define FLOAT_SIN_CACHE_SCALE 10
#define FLOAT_TAN_CACHE_SCALE 1

#define HALF_WIDTH_BUFFERS 0
#define FIELD_BUFFERS 0
#define SSR_FIELD_REFLECT 0

#define POSTFX_CRT 0
#define POSTFX_CELLSHADING 0
#define POSTFX_ANTIALIASING 0
#define POSTFX_BLOOM 0
#define POSTFX_MOTION_BLUR 0
#define POSTFX_CHROMATIC 0
#define POSTFX_PIXELATE 0
#define CRT_SCANLINE_INTENSITY 48
#define MOTION_BLUR_STRENGTH 50
#define CHROMATIC_OFFSET 2
#define PIXELATE_SIZE 4
#define CELLSHADING_CELL_BITS 4

#define DEBUG_OVERDRAW 0
#define zBrightFar 1600
#define zBrightNear 200
#define zBrightScale 48
#define depthFogFar 8192
#define depthFogNear 6144

#define CHECKERBOARD_MODE 0
#define CHECKERBOARD_RECONSTRUCTION 0
#define MAX_PICK_QUERIES 0

#if defined(ESP_PLATFORM)
#define ESP32
// Nothing of Jet's in internal RAM while no stage is open: its code runs from flash through the
// cache (it was 22 KB of IRAM), and its trig tables (17 KB) are allocated by the first scene (in
// PSRAM: allocations over 64 bytes go there; they're read a few times an object a frame).
#define PERF_CRITICAL
#endif
