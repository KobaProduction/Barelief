/* SPDX-License-Identifier: Apache-2.0
 * Exact bit reversal of complex16 words. Unlike EE.BITREV (3..10 bits),
 * parallel SWAR reverses16 bits then keeps log2N bits, covering N<=8192.
 * data16B aligned, N>=8 power-of-two. No arithmetic on signal samples.
 * Each gathered index is max(i,reverse(i)): an already swapped pair becomes
 * a self-copy when visited again. Complex words within one4-word load block
 * cannot be distinct reversal partners for log2N>=3.
 * QR/SAR caller-clobbered. Frame64 reserves its upper32 bytes for window ABI.
 */
    .section .rodata.gmf_permutation_constants,"a"
    .balign 16
.Lperm_m1: .long 0x55555555,0x55555555,0x55555555,0x55555555
.Lperm_m2: .long 0x33333333,0x33333333,0x33333333,0x33333333
.Lperm_m4: .long 0x0f0f0f0f,0x0f0f0f0f,0x0f0f0f0f,0x0f0f0f0f
.Lperm_m8: .long 0x00ff00ff,0x00ff00ff,0x00ff00ff,0x00ff00ff
.Lperm_indices: .short 0,1,2,3,4,5,6,7
    .text
    .literal .Lperm_constants,.Lperm_m1
.macro perm_swap_bits mask,amount
    movi a8,\amount
    ssr a8
    ee.vsr.32 q1,q0
    ee.andq q1,q1,\mask
    ee.andq q0,q0,\mask
    ee.vsl.32 q0,q0
    ee.orq q0,q0,q1
.endm
    .align 4
    .global gmf_s3_bit_reverse_s16
    .type gmf_s3_bit_reverse_s16,@function
gmf_s3_bit_reverse_s16:
    entry sp,64
    l32r a8,.Lperm_constants
    ee.vld.128.ip q3,a8,16
    ee.vld.128.ip q4,a8,16
    ee.vld.128.ip q5,a8,16
    ee.vld.128.ip q7,a8,16
    ee.vld.128.ip q6,a8,0
    addi a9,a3,-1
    slli a10,a9,16
    or a9,a9,a10
    s32i a9,sp,16
    movi a9,0x00080008
    s32i a9,sp,20
    movi a8,16
    sub a4,a8,a4
    srli a3,a3,3
    mov a6,a2
    mov a7,a2
    loopgtz a3,.Lperm_end
        ee.orq q0,q6,q6
        perm_swap_bits q3,1
        perm_swap_bits q4,2
        perm_swap_bits q5,4
        perm_swap_bits q7,8
        ssr a4
        ee.vsr.32 q0,q0
        addi a8,sp,16
        ee.vldbc.32 q2,a8
        ee.andq q0,q0,q2
        ee.vmax.s16 q0,q0,q6
        ee.vld.128.ip q1,a7,16
        .irp lane,0,1,2,3
            ee.ldxq.32 q2,q0,a2,\lane,\lane
        .endr
        .irp lane,0,1,2,3
            ee.stxq.32 q1,q0,a2,\lane,\lane
        .endr
        ee.vst.128.ip q2,a6,16
        ee.vld.128.ip q1,a7,16
        .irp lane,0,1,2,3
            ee.ldxq.32 q2,q0,a2,\lane,(4+\lane)
        .endr
        .irp lane,0,1,2,3
            ee.stxq.32 q1,q0,a2,\lane,(4+\lane)
        .endr
        ee.vst.128.ip q2,a6,16
        addi a8,sp,20
        ee.vldbc.32 q2,a8
        ee.vadds.s16 q6,q6,q2
.Lperm_end:
    retw
    .size gmf_s3_bit_reverse_s16,.-gmf_s3_bit_reverse_s16
