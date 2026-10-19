// RGB565 pixels as the AMOLED panel takes them: big-endian, and a scanline row (every third, the
// CRT look) dimmed by a quarter. Used by the full-colour layer (engine/hd.cpp): one formula, one
// vector loop, wherever pixels leave for the panel.
#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__XTENSA__)
#include "sdkconfig.h"  // CONFIG_IDF_TARGET_ESP32S3: the vector finish
#endif

namespace hg {

// A scanline pixel: each channel to floor(3/4 of it). The S3's vector unit does exactly this, a
// 16-bit multiply and shift per channel (finish_run below); every path uses this one formula.
// (It was floor(c/2) + floor(c/4) until 2026-10-14, which needs a plain 16-bit add the vector
// unit lacks; the two differ by one step for channel values ending in binary 11.)
__attribute__((always_inline)) inline uint16_t scan_dim(uint16_t v) {
  return static_cast<uint16_t>((((v & 0xF800u) * 3 >> 2) & 0xF800u) | (((v & 0x07E0u) * 3 >> 2) & 0x07E0u) |
                               ((v & 0x001Fu) * 3 >> 2));
}
__attribute__((always_inline)) inline uint16_t panel_order(uint16_t v) { return static_cast<uint16_t>(v << 8 | v >> 8); }

// A run of pixels to the panel's format: scanline-dimmed if `dark`, then big-endian. On the S3
// the 16-byte-aligned middle goes eight pixels an instruction through its vector unit (PIE: a
// hardware loop, no branches; the trick from CubeCoders' Jet renderer, MIT): ~7 cycles a pixel
// scalar, measured, against well under one. The byte swap unzips each pixel's low and high bytes
// and zips them back the other way round. Elsewhere (the desktop preview) a pixel at a time.
// The vector loads and stores round addresses down to 16 bytes: only whole aligned blocks go.
__attribute__((always_inline)) inline void finish_run(uint16_t* p, int n, bool dark) {
#if defined(__XTENSA__) && defined(CONFIG_IDF_TARGET_ESP32S3)
  while (n > 0 && (reinterpret_cast<uintptr_t>(p) & 15)) {
    *p = panel_order(dark ? scan_dim(*p) : *p);
    ++p;
    --n;
  }
  const int blocks = n >> 3;
  if (blocks > 0) {
    if (!dark) {
      __asm__ volatile(
          "loopgtz %[n], 1f\n\t"
          "ee.vld.128.ip q0, %[p], 0\n\t"
          "ee.orq q1, q0, q0\n\t"
          "ee.vunzip.8 q0, q1\n\t"
          "ee.vzip.8 q1, q0\n\t"
          "ee.vst.128.ip q1, %[p], 16\n\t"
          "1:\n\t"
          : [p] "+r"(p)
          : [n] "r"(blocks)
          : "memory");
    } else {
      // q4..q6: the channel masks, q7: 3 in every lane; SAR = 2, so vmul.u16 gives (c * 3) >> 2.
      alignas(16) static const uint16_t kDim[32] = {
          0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0xF800, 0x07E0, 0x07E0, 0x07E0,
          0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x07E0, 0x001F, 0x001F, 0x001F, 0x001F, 0x001F, 0x001F,
          0x001F, 0x001F, 3, 3, 3, 3, 3, 3, 3, 3};
      const uint16_t* k = kDim;
      uint32_t saved;
      __asm__ volatile(
          "rsr %[saved], sar\n\t"
          "wsr %[two], sar\n\t"
          "ee.vld.128.ip q4, %[k], 16\n\t"
          "ee.vld.128.ip q5, %[k], 16\n\t"
          "ee.vld.128.ip q6, %[k], 16\n\t"
          "ee.vld.128.ip q7, %[k], 0\n\t"
          "loopgtz %[n], 1f\n\t"
          "ee.vld.128.ip q0, %[p], 0\n\t"
          "ee.andq q1, q0, q4\n\t"
          "ee.vmul.u16 q1, q1, q7\n\t"
          "ee.andq q1, q1, q4\n\t"
          "ee.andq q2, q0, q5\n\t"
          "ee.vmul.u16 q2, q2, q7\n\t"
          "ee.andq q2, q2, q5\n\t"
          "ee.andq q3, q0, q6\n\t"
          "ee.vmul.u16 q3, q3, q7\n\t"
          "ee.orq q1, q1, q2\n\t"
          "ee.orq q1, q1, q3\n\t"
          "ee.orq q2, q1, q1\n\t"
          "ee.vunzip.8 q1, q2\n\t"
          "ee.vzip.8 q2, q1\n\t"
          "ee.vst.128.ip q2, %[p], 16\n\t"
          "1:\n\t"
          "wsr %[saved], sar\n\t"
          : [p] "+r"(p), [k] "+r"(k), [saved] "=&r"(saved)
          : [n] "r"(blocks), [two] "r"(2)
          : "memory");
    }
    n &= 7;  // the asm moved p past the blocks
  }
#endif
  for (; n > 0; --n, ++p) *p = panel_order(dark ? scan_dim(*p) : *p);
}

}  // namespace hg
