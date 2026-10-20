// The 3D stage's span loop in the ESP32-S3's vector instructions (PIE: 128-bit registers, eight
// 16-bit lanes): eight pixels of a triangle's row at a time, depth-tested against the piece's depth
// row and colored. Bit for bit what stage.cpp's scalar loop gives (stage_pie_selftest checks that
// on the board). Also the fills band() starts each piece with.
//
// Both rows aligned to 16 bytes, whole groups of 8 pixels. Depth: 24.8 in 32-bit lanes, stepped
// 8 pixels a group; its whole part (16 bits) unzipped into 16-bit lanes, tested unsigned (both sides
// biased by 0x8000 for the signed compare and min): the new depth row is the lanes' minimum, and
// where the new pixel was nearer its color is taken. Color: a flat triangle's one color, else
// the material's table at the brightness (16.16): one broadcast when the 8 pixels share an entry,
// else looked up one by one (the S3 has no vector gather).
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32S3

// PieSpan (stage.cpp), byte offsets
#define S_Z      0    /* int32 z[8]: the group's depths */
#define S_DZ8    32   /* int32: 8 x depth per pixel */
#define S_BIAS   36   /* 0x80008000 */
#define S_COLOR 40   /* a flat triangle's color, in both halves */
#define S_FLAT   44
#define S_GATHER 48   /* uint16 [8]: scratch for the looked-up colors */
#define S_ZR     64
#define S_CR     68
#define S_N8     72
#define S_B      76
#define S_DBX    80
#define S_LUT    84

    // The depth test and the blend for 8 pixels: q0 q1 depths (32-bit lanes), q2 8 x their step,
    // q3 the bias, q4 the color; a5 the depth row, a6 the color row (both advanced). SAR = 8.
    .macro TEST8
    ee.vsr.32 q5, q0                // depth >> 8
    ee.vsr.32 q6, q1
    ee.vunzip.16 q5, q6             // q5: the 8 low halves (the depths), in order
    ee.vld.128.ip q6, a5, 0         // the depth row's
    ee.xorq q5, q5, q3
    ee.xorq q6, q6, q3
    ee.vcmp.lt.s16 q7, q5, q6       // nearer: all ones
    ee.vmin.s16 q5, q5, q6
    ee.xorq q5, q5, q3
    ee.vst.128.ip q5, a5, 16
    ee.vld.128.ip q6, a6, 0         // the color row's
    ee.andq q5, q4, q7
    ee.notq q7, q7
    ee.andq q6, q6, q7
    ee.orq  q6, q6, q5
    ee.vst.128.ip q6, a6, 16
    ee.vadds.s32 q0, q0, q2
    ee.vadds.s32 q1, q1, q2
    .endm

// void stage_pie_span(PieSpan* s)
    .align  4
    .global stage_pie_span
    .type   stage_pie_span, @function
stage_pie_span:
    entry   a1, 32
    mov     a3, a2
    ee.vld.128.ip q0, a3, 16        // depths, pixels 0-3
    ee.vld.128.ip q1, a3, 16        // 4-7
    addi    a3, a2, S_DZ8
    ee.vldbc.32 q2, a3
    addi    a3, a2, S_BIAS
    ee.vldbc.32 q3, a3
    l32i    a5, a2, S_ZR
    l32i    a6, a2, S_CR
    l32i    a7, a2, S_N8
    movi    a3, 8
    wsr.sar a3
    beqz    a7, .Lspan_done
    l32i    a3, a2, S_FLAT
    beqz    a3, .Lspan_lit
    addi    a3, a2, S_COLOR
    ee.vldbc.32 q4, a3
    loopnez a7, .Lspan_flat_end
    TEST8
.Lspan_flat_end:
    j       .Lspan_done

.Lspan_lit:
    l32i    a8, a2, S_B
    l32i    a9, a2, S_DBX
    l32i    a10, a2, S_LUT
    addi    a13, a2, S_GATHER
    slli    a14, a9, 3              // 8 x the brightness step
    sub     a11, a14, a9            // 7 x
.Lspan_group:
    srai    a4, a8, 16              // the first pixel's entry
    add     a3, a8, a11
    srai    a3, a3, 16              // the last's
    bne     a3, a4, .Lspan_gather
    addx2   a3, a4, a10
    ee.vldbc.16 q4, a3              // one entry for all 8
    j       .Lspan_test
.Lspan_gather:
    mov     a3, a8
    .irp    i, 0, 2, 4, 6, 8, 10, 12, 14
    srai    a4, a3, 16
    addx2   a4, a4, a10
    l16ui   a4, a4, 0
    s16i    a4, a13, \i
    add     a3, a3, a9
    .endr
    ee.vld.128.ip q4, a13, 0
.Lspan_test:
    TEST8
    add     a8, a8, a14
    addi    a7, a7, -1
    bnez    a7, .Lspan_group
.Lspan_done:
    retw.n
    .size   stage_pie_span, . - stage_pie_span

// void stage_pie_fill(void* p, int n16, uint32_t v): n16 groups of 16 bytes at p (aligned), each word v
    .align  4
    .global stage_pie_fill
    .type   stage_pie_fill, @function
stage_pie_fill:
    entry   a1, 32
    ee.movi.32.q q0, a4, 0
    ee.movi.32.q q0, a4, 1
    ee.movi.32.q q0, a4, 2
    ee.movi.32.q q0, a4, 3
    loopnez a3, .Lfill_end
    ee.vst.128.ip q0, a2, 16
.Lfill_end:
    retw.n
    .size   stage_pie_fill, . - stage_pie_fill

#endif
