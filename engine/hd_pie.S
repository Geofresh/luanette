// The full-colour layer's blend in the ESP32-S3's vector instructions (PIE: 128-bit registers,
// eight 16-bit lanes): front rows blended over what the panel row has, by their alpha. Bit for
// bit what hd.cpp's scalar code gives (compose()'s over_px): hd_pie_selftest() checks that on the
// board. (Baking rows for the panel is hg::finish_run, support/hg/panel_pixels.hpp.)
//
// It takes pointers aligned to 16 bytes and whole groups of 8 pixels, and k: the constants below
// (kPie in hd.cpp), each 32-bit word broadcast across a register. SAR is the vector shifts' and
// multiplies' shift.
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

#define K_00FF 0   /* 0x00FF00FF */
#define K_FF00 4   /* 0xFF00FF00 */
#define K_RB   8   /* 0xF81FF81F: red and blue's fields */
#define K_G    12  /* 0x07E007E0: green's */
#define K_3    16  /* 0x00030003 */
#define K_1F   20  /* 0x001F001F */
#define K_3F   24  /* 0x003F003F */
#define K_4    28  /* 0x00040004 */
#define K_F800 32  /* 0xF800F800 */

    .macro K q, off            // \q: the constant at k + \off, in every lane (k in a12)
    addi    a13, a12, \off
    ee.vldbc.32 \q, a13
    .endm

    .macro SAR n
    movi    a14, \n
    wsr.sar a14
    .endm

    // \q's 16-bit lanes byte-swapped (big-endian <-> native), \t1 \t2 spare; leaves SAR 8.
    .macro SWAP q, t1, t2
    SAR     8
    ee.vsl.32 \t1, \q
    ee.vsr.32 \t2, \q
    K       \q, K_FF00
    ee.andq \t1, \t1, \q
    K       \q, K_00FF
    ee.andq \t2, \t2, \q
    ee.orq  \q, \t1, \t2
    .endm

    // \q's native pixels a quarter darker: each channel x 3 / 4 (floored), in their fields (red and
    // blue's x 3 >> 2 don't meet: red's lands from bit 9, blue's below 5), \t1-\t3 spare.
    .macro DIM q, t1, t2, t3
    K       \t3, K_RB
    ee.andq \t1, \q, \t3
    K       \t2, K_G
    ee.andq \t2, \q, \t2
    K       \q, K_3
    SAR     2
    ee.vmul.u16 \t1, \t1, \q
    ee.vmul.u16 \t2, \t2, \q
    ee.andq \t1, \t1, \t3
    K       \t3, K_G
    ee.andq \t2, \t2, \t3
    ee.orq  \q, \t1, \t2
    .endm

    // 8 pixels: s from a3 (native), alphas from a4, d from a11 (big-endian), the blend to a2.
    // Each channel d + ((s - d) * t >> 5), t = (alpha + 4) >> 3 (0-32): the scalar's
    // (s * t + d * (32 - t)) >> 5 exactly; alpha 0 leaves d, 255 gives s.
    .macro OVER8 dim
    ee.vld.128.ip q0, a3, 16        // s
    ee.vld.128.ip q1, a11, 16       // d, big-endian
    ee.zero.q q2
    ee.vld.l.64.ip q2, a4, 8        // 8 alphas, in the low half
    ee.zero.q q3
    ee.vzip.8 q2, q3                // each in a 16-bit lane
    K       q3, K_4
    ee.vadds.s16 q2, q2, q3
    SAR     3
    ee.vsr.32 q2, q2
    K       q3, K_3F
    ee.andq q2, q2, q3              // t
    .if \dim
    DIM     q0, q3, q4, q5
    .endif
    SWAP    q1, q3, q4              // d, native
    K       q6, K_1F                // blue
    ee.andq q3, q0, q6
    ee.andq q4, q1, q6
    ee.vsubs.s16 q3, q3, q4
    SAR     5
    ee.vmul.s16 q3, q3, q2
    ee.vadds.s16 q5, q4, q3
    ee.vsr.32 q3, q0                // green (SAR 5)
    ee.vsr.32 q4, q1
    K       q6, K_3F
    ee.andq q3, q3, q6
    ee.andq q4, q4, q6
    ee.vsubs.s16 q3, q3, q4
    ee.vmul.s16 q3, q3, q2
    ee.vadds.s16 q3, q4, q3
    ee.vsl.32 q3, q3
    K       q6, K_G
    ee.andq q3, q3, q6
    ee.orq  q5, q5, q3
    SAR     11                      // red
    ee.vsr.32 q3, q0
    ee.vsr.32 q4, q1
    K       q6, K_1F
    ee.andq q3, q3, q6
    ee.andq q4, q4, q6
    ee.vsubs.s16 q3, q3, q4
    SAR     5
    ee.vmul.s16 q3, q3, q2
    ee.vadds.s16 q3, q4, q3
    SAR     11
    ee.vsl.32 q3, q3
    K       q6, K_F800
    ee.andq q3, q3, q6
    ee.orq  q5, q5, q3
    SWAP    q5, q3, q4              // big-endian again
    ee.vst.128.ip q5, a2, 16
    .endm

// void hd_pie_over(uint16_t* out, const uint16_t* src, const uint8_t* alpha, int n8, const uint32_t* k, int dim)
// out[i] (big-endian) blended toward src[i] (native; dimmed if dim) by alpha[i]: n8 groups of 8.
    .align  4
    .global hd_pie_over
    .type   hd_pie_over, @function
hd_pie_over:
    entry   a1, 32
    mov     a12, a6
    mov     a11, a2
    beqz    a5, .Lover_done
    beqz    a7, .Lover_plain
    // A group whose 8 alphas are all 0 leaves out as it is: skipped (a HUD is mostly clear between
    // its pieces). Its alphas are two words (a4: 8-byte steps from 16-aligned).
.Lover_dim:
    l32i    a8, a4, 0
    l32i    a9, a4, 4
    or      a8, a8, a9
    beqz    a8, .Lover_dim_skip
    OVER8   1
    addi    a5, a5, -1
    bnez    a5, .Lover_dim
    j       .Lover_done
.Lover_dim_skip:
    addi    a3, a3, 16
    addi    a4, a4, 8
    addi    a11, a11, 16
    addi    a2, a2, 16
    addi    a5, a5, -1
    bnez    a5, .Lover_dim
    j       .Lover_done
.Lover_plain:
    l32i    a8, a4, 0
    l32i    a9, a4, 4
    or      a8, a8, a9
    beqz    a8, .Lover_plain_skip
    OVER8   0
    addi    a5, a5, -1
    bnez    a5, .Lover_plain
    j       .Lover_done
.Lover_plain_skip:
    addi    a3, a3, 16
    addi    a4, a4, 8
    addi    a11, a11, 16
    addi    a2, a2, 16
    addi    a5, a5, -1
    bnez    a5, .Lover_plain
.Lover_done:
    retw.n
    .size   hd_pie_over, . - hd_pie_over

#endif
