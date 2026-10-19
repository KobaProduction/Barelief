/*
 * SPDX-FileCopyrightText: 2018-2025 Espressif Systems (Shanghai) CO LTD
 * SPDX-FileContributor: 2024 f4lc0n @ Libera Chat IRC
 *
 * SPDX-License-Identifier: Apache-2.0
 */


// Adapted from ESP-DSP a53a0756833c045311ea1d79a2badf495cdfde4c.
// Exact unity groups, streamed final stage and S3 paired Float32 loads/stores.
#include "dsps_fft2r_platform.h"
#if (dsps_fft2r_fc32_aes3_enabled == 1)

	.section .iram1.dsp_rfft_fft2r_aes3,"ax"
	.align  4
	.global dsp_rfft_fft2r_aes3_
	.type   dsp_rfft_fft2r_aes3_,@function

// The function implements the following C code:
//esp_err_t dsps_fft2r_fc32_ansi(float *data, int N)
//{
//    float *w = dsps_fft_w_table_fc32;
//
//    int ie, ia, m;
//    float re_temp, im_temp;
//    float c, s;
//    int N2 = N;
//    ie = 1;
//    for (int N2 = N/2; N2 > 0; N2 >>= 1) {
//        ia = 0;
//        for (int j = 0; j < ie; j++) {
//            c = w[2 * j];
//            s = w[2 * j + 1];
//            for (int i = 0; i < N2; i++) {
//                m = ia + N2;
//                re_temp = c * data[2 * m] + s * data[2 * m + 1];
//                im_temp = c * data[2 * m + 1] - s * data[2 * m];
//                data[2 * m] = data[2 * ia] - re_temp;
//                data[2 * m + 1] = data[2 * ia + 1] - im_temp;
//                data[2 * ia] = data[2 * ia] + re_temp;
//                data[2 * ia + 1] = data[2 * ia + 1] + im_temp;
//                ia++;
//            }
//            ia += N2;
//        }
//        ie <<= 1;
//    }
//    return result;
//}


dsp_rfft_fft2r_aes3_:
//esp_err_t dsps_fft2r_fc32_ansi(float *data, int N, float* dsps_fft_w_table_fc32)

	entry	a1, 16

// Array increment for floating point data should be 4
// data - a2
// N - a3
// dsps_fft_w_table_fc32 - a4

// a6 - k, main loop counter; N2 - for (int N2 = N/2; N2 > 0; N2 >>= 1)
// a7 - ie
// a8 - j
// a10 - (j*2)<<2,  or a10 - j<<3
// f0 - c or w[2 * j]
// f1 - s or w[2 * j + 1]
// a11 - ia
// a12 - m
// a13 - ia pointer
// a14 - m pointer
// f6  - re_temp
// f7  - im_temp

    srli a6,a3,1
    // First stage has exactly one twiddle, 1+0i, for every butterfly.
    mov.n a13,a2
    addx8 a14,a6,a2
    loopnez a6,.unity_end
    ee.ldf.64.ip f3,f2,a13,0
    ee.ldf.64.ip f5,f4,a14,0
    add.s f10,f2,f4
    sub.s f8,f2,f4
    add.s f11,f3,f5
    sub.s f9,f3,f5
    ee.stf.64.ip f11,f10,a13,8
    ee.stf.64.ip f9,f8,a14,8
.unity_end:
    srli a6,a6,1
    movi.n a7,2
    beqz a6,.fft_done

.fft2r_l1:
    // The last stage has N2=1: stream adjacent pairs and coefficients.
    movi.n a8,1
    beq a6,a8,.fft2r_final
    // j=0 is an exact unity rotation in every stage, not only the first.
    mov.n a13,a2
    addx8 a14,a6,a2
    loopnez a6,.stage_unity_end
    ee.ldf.64.ip f3,f2,a13,0
    ee.ldf.64.ip f5,f4,a14,0
    add.s f10,f2,f4
    sub.s f8,f2,f4
    add.s f11,f3,f5
    sub.s f9,f3,f5
    ee.stf.64.ip f11,f10,a13,8
    ee.stf.64.ip f9,f8,a14,8
.stage_unity_end:
    slli a11,a6,1  // j=1 starts after the unity group.

.fft2r_l2:         // loop for j, a8 - j
        addx8   a10, a8, a4 // a8 - shift for cos () -- c = w[2 * j]; -- pointer to cos
        ee.ldf.64.ip f1,f0,a10,0

        // Pointers move through a group; no per-butterfly index arithmetic.
        add.n a12,a11,a6
        addx8 a14,a12,a2
        addx8 a13,a11,a2
        loopnez a6,.fft2r_l3
            ee.ldf.64.ip f5,f4,a14,0
            mul.s f6,f0,f4
            mul.s f7,f0,f5
            ee.ldf.64.ip f3,f2,a13,0
            madd.s f6,f1,f5
            msub.s f7,f1,f4
            sub.s f8,f2,f6
            add.s f10,f2,f6
            sub.s f9,f3,f7
            add.s f11,f3,f7
            ee.stf.64.ip f9,f8,a14,8
            ee.stf.64.ip f11,f10,a13,8
.fft2r_l3:
        addx2 a11,a6,a11

        addi.n  a8, a8, 1 // j++
        bne     a8, a7, .fft2r_l2
    slli    a7, a7, 1  // ie = ie<<1
// main loop: for (int k = N/2; k > 0; k >>= 1)
    srli    a6, a6, 1     // a6 = a6>>1
    bnez    a6, .fft2r_l1 // Jump if > 0
    j .fft_done

.fft2r_final:
    loopnez a7,.final_end
    ee.ldf.64.ip f1,f0,a4,8
    addi.n a14,a2,8
    ee.ldf.64.ip f5,f4,a14,0
    mul.s f6,f0,f4
    mul.s f7,f0,f5
    ee.ldf.64.ip f3,f2,a2,0
    madd.s f6,f1,f5
    msub.s f7,f1,f4
    sub.s f8,f2,f6
    add.s f10,f2,f6
    sub.s f9,f3,f7
    add.s f11,f3,f7
    ee.stf.64.ip f9,f8,a14,0
    ee.stf.64.ip f11,f10,a2,16
.final_end:

.fft_done:
	movi.n	a2, 0 // return status ESP_OK
	retw.n
    .size dsp_rfft_fft2r_aes3_,.-dsp_rfft_fft2r_aes3_

#endif // dsps_fft2r_fc32_aes3_enabled
