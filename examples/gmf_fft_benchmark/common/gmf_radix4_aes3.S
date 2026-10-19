/* SPDX-FileCopyrightText: 2026 Contributors to gmf_fft
 * SPDX-License-Identifier: Apache-2.0
 * Four true radix4 butterflies, with signed32 sums and exact wide products.
 * Wide products retain quotient, low bits and residual. Checked signed16
 * inputs use exact high/low reconstruction, or direct unrounded CMUL when safe.
 * No18-bit butterfly sum is silently narrowed to signed16.
 * See gmf_radix4_model.c for the exact identity and host semantic model.
 * ctx: a,b,c,d at0..12; W1,W2,W3 at16..24; S at28; rounding/4 at32.
 *128-bit transfers are16B aligned; small8 uses8B-aligned64-bit transfers.
 * Direct real A uses USAR+SRC for4B alignment. QR/SAR/SAR_BYTE are caller-clobbered.
 */
    .section .rodata.gmf_r4_constants,"a"
    /* Xtensa .align takes bytes: .align 4 is NOT a16-byte vector alignment.
     * The section must carry an explicit16-byte alignment through linking. */
    .balign 16
.Lr4_mask16: .long 65535,65535,65535,65535
.Lr4_mask3:  .long 3,3,3,3
.Lr4_mask15: .long 0x7fff7fff,0x7fff7fff,0x7fff7fff,0x7fff7fff
.Lr4_min:    .long -32768,-32768,-32768,-32768
.Lr4_max:    .long 32767,32767,32767,32767
.Lr4_even:   .long -1,0,-1,0
    .text
    .literal .Lr4_mask16_ptr,.Lr4_mask16
    .literal .Lr4_mask3_ptr,.Lr4_mask3
    .literal .Lr4_mask15_ptr,.Lr4_mask15
    .literal .Lr4_min_ptr,.Lr4_min
    .literal .Lr4_max_ptr,.Lr4_max
    .literal .Lr4_even_ptr,.Lr4_even
.macro r4_const qr,ptr
    l32r a9,\ptr
    ee.vld.128.ip \qr,a9,0
.endm
/* q3=real, q2=imag, signed32x4. */
.macro r4_load field
    l32i a8,a2,\field
    ee.vld.128.ip q2,a8,0
    movi a8,16
    ssr a8
    ee.vsl.32 q3,q2
    ee.vsr.32 q3,q3
    ee.vsr.32 q2,q2
.endm
.macro r4_pack real,imag
    r4_const q3,.Lr4_min_ptr
    ee.vmax.s32 \real,\real,q3
    ee.vmax.s32 \imag,\imag,q3
    r4_const q3,.Lr4_max_ptr
    ee.vmin.s32 \real,\real,q3
    ee.vmin.s32 \imag,\imag,q3
    r4_const q4,.Lr4_mask16_ptr
    ee.andq \real,\real,q4
    movi a8,16
    ssr a8
    ee.vsl.32 \imag,\imag
    ee.orq \real,\real,\imag
.endm
/* Input q0=wide real, q1=wide imag. Output q1=packed saturated complex16x4.
 * P=(x>>2)*W, L=(x&3)*W. CMUL SAR15 coarse fits16; SAR0 low bits wrap
 * intentionally modulo65536; SAR2 residual fits16. Reconstruction is signed32.
 */
.macro r4_product_wide twfield,inverse,packed=0
    /* Check signed16 representability without an abs(-32768) overflow.
     * A mixed batch takes the original exact wide path for all four lanes. */
    movi a8,16
    ssr a8
    ee.vsl.32 q2,q0
    ee.vsr.32 q2,q2
    ee.xorq q2,q2,q0
    ee.vsl.32 q3,q1
    ee.vsr.32 q3,q3
    ee.xorq q3,q3,q1
    ee.orq q2,q2,q3
    ee.movi.32.a q2,a8,0
    ee.movi.32.a q2,a9,1
    or a8,a8,a9
    ee.movi.32.a q2,a4,2
    or a8,a8,a4
    ee.movi.32.a q2,a5,3
    or a8,a8,a5
    bnez a8,.Lr4_wide\@
    r4_const q4,.Lr4_mask16_ptr
    ee.andq q2,q0,q4
    ee.vsl.32 q3,q1
    ee.orq q2,q2,q3
    l32i a8,a2,\twfield
    ee.vld.128.ip q4,a8,0
    .if \packed
        /* For S>=15 the Q14 L1 bound guarantees a signed16 product output.
         * With zero bias, direct CMUL already is the complete exact result. */
        l32i a4,a2,28
        l32i a5,a2,32
        bnez a5,.Lr4_narrow_reconstruct\@
        movi a8,15
        blt a4,a8,.Lr4_narrow_reconstruct\@
        ssr a4
        .if \inverse
            ee.cmul.s16 q1,q4,q2,0
            ee.cmul.s16 q1,q4,q2,1
        .else
            ee.cmul.s16 q1,q4,q2,2
            ee.cmul.s16 q1,q4,q2,3
        .endif
        j .Lr4_product_done\@
    .endif
.Lr4_narrow_reconstruct\@:
    /* P fits signed32: |P|<=32768*23172. Recover every product bit from
     * signed high16 (SAR16) and wrapped low16 (SAR0), then bias/shift once.
     * S14 still needs final saturation; real pairs keep the wide output. */
    movi a8,16
    ssr a8
    .if \inverse
        ee.cmul.s16 q5,q4,q2,0
        ee.cmul.s16 q5,q4,q2,1
    .else
        ee.cmul.s16 q5,q4,q2,2
        ee.cmul.s16 q5,q4,q2,3
    .endif
    movi a8,0
    ssr a8
    .if \inverse
        ee.cmul.s16 q6,q4,q2,0
        ee.cmul.s16 q6,q4,q2,1
    .else
        ee.cmul.s16 q6,q4,q2,2
        ee.cmul.s16 q6,q4,q2,3
    .endif
    movi a8,16
    ssr a8
    ee.vsl.32 q1,q5
    ee.vsr.32 q7,q6
    r4_const q4,.Lr4_mask16_ptr
    ee.andq q2,q6,q4
    ee.orq q1,q1,q2
    ee.andq q2,q5,q4
    ee.xorq q0,q5,q2
    ee.andq q7,q7,q4
    ee.orq q0,q0,q7
    addi a8,a2,32
    ee.vldbc.32 q4,a8
    movi a8,2
    ssr a8
    ee.vsl.32 q4,q4
    ee.vadds.s32 q1,q1,q4
    ee.vadds.s32 q0,q0,q4
    l32i a4,a2,28
    ssr a4
    ee.vsr.32 q1,q1
    ee.vsr.32 q0,q0
    .if \packed
        r4_pack q1,q0
    .endif
    j .Lr4_product_done\@
.Lr4_wide\@:
    r4_const q4,.Lr4_mask16_ptr
    movi a8,2
    ssr a8
    ee.vsr.32 q2,q0
    ee.vsr.32 q3,q1
    ee.andq q2,q2,q4
    movi a8,16
    ssr a8
    ee.vsl.32 q3,q3
    ee.orq q2,q2,q3
    r4_const q4,.Lr4_mask3_ptr
    ee.andq q0,q0,q4
    ee.andq q1,q1,q4
    ee.vsl.32 q1,q1
    ee.orq q3,q0,q1
    l32i a8,a2,\twfield
    ee.vld.128.ip q4,a8,0
    movi a8,15
    ssr a8
    .if \inverse
        ee.cmul.s16 q5,q4,q2,0
        ee.cmul.s16 q5,q4,q2,1
    .else
        ee.cmul.s16 q5,q4,q2,2
        ee.cmul.s16 q5,q4,q2,3
    .endif
    movi a8,0
    ssr a8
    .if \inverse
        ee.cmul.s16 q6,q4,q2,0
        ee.cmul.s16 q6,q4,q2,1
    .else
        ee.cmul.s16 q6,q4,q2,2
        ee.cmul.s16 q6,q4,q2,3
    .endif
    movi a8,2
    ssr a8
    .if \inverse
        ee.cmul.s16 q7,q4,q3,0
        ee.cmul.s16 q7,q4,q3,1
    .else
        ee.cmul.s16 q7,q4,q3,2
        ee.cmul.s16 q7,q4,q3,3
    .endif
    movi a8,16
    ssr a8
    ee.vsr.32 q0,q5
    ee.vsl.32 q1,q5
    ee.vsr.32 q1,q1
    l32i a4,a2,28
    movi a5,17
    sub a5,a5,a4
    ssr a5
    ee.vsl.32 q0,q0
    ee.vsl.32 q1,q1
    r4_const q4,.Lr4_mask15_ptr
    ee.andq q6,q6,q4
    r4_const q4,.Lr4_mask16_ptr
    ee.andq q2,q6,q4
    movi a8,16
    ssr a8
    ee.vsr.32 q6,q6
    ee.vsl.32 q3,q7
    ee.vsr.32 q3,q3
    ee.vsr.32 q7,q7
    ee.vadds.s32 q2,q2,q3
    ee.vadds.s32 q6,q6,q7
    addi a8,a2,32
    ee.vldbc.32 q4,a8
    ee.vadds.s32 q2,q2,q4
    ee.vadds.s32 q6,q6,q4
    addi a5,a4,-2
    ssr a5
    ee.vsr.32 q2,q2
    ee.vsr.32 q6,q6
    ee.vadds.s32 q1,q1,q2
    ee.vadds.s32 q0,q0,q6
    .if \packed
        r4_pack q1,q0
    .endif
.Lr4_product_done\@:
.endm
.macro r4_product twfield,inverse
    r4_product_wide \twfield,\inverse,1
.endm
.macro r4_input field,real,imag,small8
    l32i a8,a2,\field
    .if \small8
        ee.vld.l.64.ip \imag,a8,0
        addi a8,a8,32
        ee.vld.h.64.ip \imag,a8,0
    .else
        ee.vld.128.ip \imag,a8,0
    .endif
    ee.vsl.32 \real,\imag
    ee.vsr.32 \real,\real
    ee.vsr.32 \imag,\imag
.endm
.macro r4_output field,qr,small8
    l32i a8,a2,\field
    .if \small8
        ee.vst.l.64.ip \qr,a8,0
        addi a8,a8,32
        ee.vst.h.64.ip \qr,a8,0
    .else
        ee.vst.128.ip \qr,a8,0
    .endif
.endm
.macro r4_body inverse,small8=0,boxes=0
    /* Wide sums at16..143, bias at144..147. Top32 remain ABI-reserved. */
    .if \boxes
        /* Local mutable context160..195, loop state196..203; top32 ABI. */
        entry sp,256
        .irp field,0,4,8,12,16,20,24,28,32
            l32i a8,a2,\field
            s32i a8,sp,(160+\field)
        .endr
        addi a2,sp,160
        s32i a3,sp,196
        s32i a4,sp,200
    .else
        entry sp,208
    .endif
.Lr4_box_start\@:
    movi a8,16
    ssr a8
    r4_input 0,q0,q1,\small8
    r4_input 8,q2,q3,\small8
    ee.vadds.s32 q4,q0,q2
    ee.vadds.s32 q5,q1,q3
    ee.vsubs.s32 q6,q0,q2
    ee.vsubs.s32 q7,q1,q3
    r4_input 4,q0,q1,\small8
    r4_input 12,q2,q3,\small8
    ee.vsubs.s32 q2,q0,q2
    ee.vsubs.s32 q3,q1,q3
    ee.vadds.s32 q0,q0,q0
    ee.vsubs.s32 q0,q0,q2
    ee.vadds.s32 q1,q1,q1
    ee.vsubs.s32 q1,q1,q3
    ee.vadds.s32 q0,q4,q0
    ee.vadds.s32 q1,q5,q1
    addi a8,sp,16
    ee.vst.128.ip q0,a8,16
    ee.vst.128.ip q1,a8,16
    ee.vadds.s32 q4,q4,q4
    ee.vsubs.s32 q4,q4,q0
    ee.vadds.s32 q5,q5,q5
    ee.vsubs.s32 q5,q5,q1
    ee.vst.128.ip q4,a8,16
    ee.vst.128.ip q5,a8,16
    .if \inverse
        ee.vsubs.s32 q0,q6,q3
        ee.vadds.s32 q1,q7,q2
    .else
        ee.vadds.s32 q0,q6,q3
        ee.vsubs.s32 q1,q7,q2
    .endif
    ee.vst.128.ip q0,a8,16
    ee.vst.128.ip q1,a8,16
    ee.vadds.s32 q6,q6,q6
    ee.vsubs.s32 q6,q6,q0
    ee.vadds.s32 q7,q7,q7
    ee.vsubs.s32 q7,q7,q1
    ee.vst.128.ip q6,a8,16
    ee.vst.128.ip q7,a8,0
    /* All inputs have been consumed before publishing any packed output. */
    addi a8,sp,16
    ee.vld.128.ip q0,a8,16
    ee.vld.128.ip q1,a8,0
    l32i a4,a2,28
    addi a4,a4,-14
    l32i a5,a2,32
    srli a5,a5,12
    s32i a5,sp,144
    addi a8,sp,144
    ee.vldbc.32 q4,a8
    ee.vadds.s32 q0,q0,q4
    ee.vadds.s32 q1,q1,q4
    ssr a4
    ee.vsr.32 q0,q0
    ee.vsr.32 q1,q1
    r4_pack q0,q1
    r4_output 0,q0,\small8
    addi a8,sp,48
    ee.vld.128.ip q0,a8,16
    ee.vld.128.ip q1,a8,0
    r4_product 20,\inverse
    r4_output 4,q1,\small8
    addi a8,sp,80
    ee.vld.128.ip q0,a8,16
    ee.vld.128.ip q1,a8,0
    r4_product 16,\inverse
    r4_output 8,q1,\small8
    addi a8,sp,112
    ee.vld.128.ip q0,a8,16
    ee.vld.128.ip q1,a8,0
    r4_product 24,\inverse
    r4_output 12,q1,\small8
    .if \boxes
        l32i a3,sp,196
        addi a3,a3,-1
        beqz a3,.Lr4_box_done\@
        s32i a3,sp,196
        l32i a4,sp,200
        .irp field,0,4,8,12
            l32i a8,a2,\field
            add a8,a8,a4
            s32i a8,a2,\field
        .endr
        j .Lr4_box_start\@
.Lr4_box_done\@:
    .endif
    retw
.endm
    .align 4
    .global gmf_r4_s3_batch_forward
    .type gmf_r4_s3_batch_forward,@function
gmf_r4_s3_batch_forward:
    r4_body 0
    .align 4
    .global gmf_r4_s3_batch_inverse
    .type gmf_r4_s3_batch_inverse,@function
gmf_r4_s3_batch_inverse:
    r4_body 1
    .align 4
    .global gmf_r4_s3_small8_forward
    .type gmf_r4_s3_small8_forward,@function
gmf_r4_s3_small8_forward:
    r4_body 0,1
    .align 4
    .global gmf_r4_s3_small8_inverse
    .type gmf_r4_s3_small8_inverse,@function
gmf_r4_s3_small8_inverse:
    r4_body 1,1
    .align 4
    .global gmf_r4_s3_boxes_forward
    .type gmf_r4_s3_boxes_forward,@function
gmf_r4_s3_boxes_forward:
    r4_body 0,0,1
    .align 4
    .global gmf_r4_s3_boxes_inverse
    .type gmf_r4_s3_boxes_inverse,@function
gmf_r4_s3_boxes_inverse:
    r4_body 1,0,1
    .align 4
    .global gmf_r4_s3_small8_boxes_forward
    .type gmf_r4_s3_small8_boxes_forward,@function
gmf_r4_s3_small8_boxes_forward:
    r4_body 0,1,1
    .align 4
    .global gmf_r4_s3_small8_boxes_inverse
    .type gmf_r4_s3_small8_boxes_inverse,@function
gmf_r4_s3_small8_boxes_inverse:
    r4_body 1,1,1

/* Final unity radix2. Keep sums/differences signed32 until the original
 * bias/shift/saturation; four contiguous complex values per iteration. */
    .align 4
    .global gmf_r4_s3_unity
    .type gmf_r4_s3_unity,@function
gmf_r4_s3_unity:
    entry sp,64
    s32i a5,sp,16
    addi a8,sp,16
    ee.vldbc.32 q7,a8
    r4_const q6,.Lr4_even_ptr
    srli a3,a3,2
    loopgtz a3,.Lr4_unity_end
        ee.vld.128.ip q1,a2,0
        movi a8,16
        ssr a8
        ee.vsl.32 q0,q1
        ee.vsr.32 q0,q0
        ee.vsr.32 q1,q1
        movi a8,4
        wur.sar_byte a8
        ee.src.q q2,q0,q0
        ee.src.q q3,q1,q1
        ee.vadds.s32 q4,q0,q2
        ee.vadds.s32 q5,q1,q3
        ee.vsubs.s32 q0,q0,q2
        ee.vsubs.s32 q1,q1,q3
        movi a8,12
        wur.sar_byte a8
        ee.src.q q0,q0,q0
        ee.src.q q1,q1,q1
        ee.andq q2,q0,q6
        ee.xorq q0,q0,q2
        ee.andq q2,q1,q6
        ee.xorq q1,q1,q2
        ee.andq q4,q4,q6
        ee.andq q5,q5,q6
        ee.orq q0,q0,q4
        ee.orq q1,q1,q5
        ee.vadds.s32 q0,q0,q7
        ee.vadds.s32 q1,q1,q7
        ssr a4
        ee.vsr.32 q0,q0
        ee.vsr.32 q1,q1
        r4_pack q0,q1
        ee.vst.128.ip q0,a2,16
.Lr4_unity_end:
    retw

/* Wide Hermitian pairs: a2=A, a3=B in conjugate index order, a4=W, a5=out,
 * a6=product rounding/4 (2048 HP,0 ordinary Q15).
 * out is SoA [t0x4,t1x4,t2x4,t3x4] signed32, without early saturation. */
.macro r4_real_load field,direct,reverse
    .if \direct
        l32i a8,a2,\field
        .if \reverse
            ee.vld.128.ip q4,a8,0
            .irp lane,0,1,2,3
                ee.movi.32.a q4,a8,(3-\lane)
                ee.movi.32.q q2,a8,\lane
            .endr
        .else
            ee.ld.128.usar.ip q2,a8,16
            ee.vld.128.ip q4,a8,0
            ee.src.q q2,q2,q4
        .endif
        movi a8,16
        ssr a8
        ee.vsl.32 q3,q2
        ee.vsr.32 q3,q3
        ee.vsr.32 q2,q2
    .else
        r4_load \field
    .endif
.endm
.macro r4_real_body inverse,direct=0,range=0,publish=0
    /* Context ends at+115; reserve the top32 bytes for window save areas. */
    entry sp,160
    s32i a2,sp,80
    s32i a3,sp,84
    .if \publish
        /* Fused API: A,B,W,product_bias/4,output_shift,output_bias. */
        s32i a6,sp,88
        s32i a7,sp,92
        s32i a5,sp,112
    .else
        s32i a5,sp,88
        s32i a6,sp,112
    .endif
    .if \range
        s32i a7,sp,92
    .endif
    s32i a4,sp,96
    movi a8,14
    s32i a8,sp,108
    addi a2,sp,80
    r4_real_load 0,\direct,0
    ee.orq q0,q3,q3
    ee.orq q1,q2,q2
    r4_real_load 4,\direct,1
    ee.vadds.s32 q4,q0,q3
    ee.vsubs.s32 q5,q1,q2
    addi a8,sp,16
    ee.vst.128.ip q4,a8,16
    ee.vst.128.ip q5,a8,0
    ee.vsubs.s32 q0,q0,q3
    ee.vadds.s32 q1,q1,q2
    r4_product_wide 16,(1-\inverse)
    addi a8,sp,16
    ee.vld.128.ip q4,a8,16
    ee.vld.128.ip q5,a8,0
    ee.vadds.s32 q2,q4,q1
    ee.vadds.s32 q3,q5,q0
    ee.vsubs.s32 q4,q4,q1
    ee.vsubs.s32 q5,q0,q5
    .if \range
        /* HP first pass needs only global extrema, not a materialized tile. */
        ee.vmin.s32 q6,q2,q3
        ee.vmin.s32 q6,q6,q4
        ee.vmin.s32 q6,q6,q5
        ee.vmax.s32 q7,q2,q3
        ee.vmax.s32 q7,q7,q4
        ee.vmax.s32 q7,q7,q5
        .irp amount,8,4
            movi a8,\amount
            wur.sar_byte a8
            ee.src.q q0,q6,q6
            ee.vmin.s32 q6,q6,q0
            ee.src.q q1,q7,q7
            ee.vmax.s32 q7,q7,q1
        .endr
        ee.movi.32.a q6,a8,0
        ee.movi.32.a q7,a9,0
        l32i a10,a2,12
        l32i a4,a10,0
        l32i a5,a10,4
        min a8,a8,a4
        max a9,a9,a5
        s32i a8,a10,0
        s32i a9,a10,4
    .elseif \publish
        /* Keep the four wide outputs in registers through quantization.
         * q3/q4 become pack scratch after these copies. */
        ee.orq q0,q2,q2
        ee.orq q1,q3,q3
        ee.orq q2,q4,q4
        addi a8,a2,12
        ee.vldbc.32 q6,a8
        ee.vadds.s32 q0,q0,q6
        ee.vadds.s32 q1,q1,q6
        ee.vadds.s32 q2,q2,q6
        ee.vadds.s32 q5,q5,q6
        l32i a4,a2,8
        ssr a4
        ee.vsr.32 q0,q0
        ee.vsr.32 q1,q1
        ee.vsr.32 q2,q2
        ee.vsr.32 q5,q5
        r4_pack q0,q1
        r4_pack q2,q5
        l32i a10,a2,0
        l32i a11,a2,4
        .irp lane,0,1,2,3
            ee.movi.32.a q0,a8,\lane
            s32i a8,a10,(4*\lane)
            ee.movi.32.a q2,a8,(3-\lane)
            ee.movi.32.q q1,a8,\lane
        .endr
        ee.vst.128.ip q1,a11,0
    .else
        l32i a8,a2,8
        ee.vst.128.ip q2,a8,16
        ee.vst.128.ip q3,a8,16
        ee.vst.128.ip q4,a8,16
        ee.vst.128.ip q5,a8,0
    .endif
    retw
.endm
    .align 4
    .global gmf_r4_s3_pairs_forward
    .type gmf_r4_s3_pairs_forward,@function
gmf_r4_s3_pairs_forward:
    r4_real_body 0
    .align 4
    .global gmf_r4_s3_pairs_inverse
    .type gmf_r4_s3_pairs_inverse,@function
gmf_r4_s3_pairs_inverse:
    r4_real_body 1
    .align 4
    .global gmf_r4_s3_pairs_direct_forward
    .type gmf_r4_s3_pairs_direct_forward,@function
gmf_r4_s3_pairs_direct_forward:
    r4_real_body 0,1
    .align 4
    .global gmf_r4_s3_pairs_direct_inverse
    .type gmf_r4_s3_pairs_direct_inverse,@function
gmf_r4_s3_pairs_direct_inverse:
    r4_real_body 1,1
    .align 4
    .global gmf_r4_s3_pairs_range_forward
    .type gmf_r4_s3_pairs_range_forward,@function
gmf_r4_s3_pairs_range_forward:
    r4_real_body 0,1,1
    .align 4
    .global gmf_r4_s3_pairs_range_inverse
    .type gmf_r4_s3_pairs_range_inverse,@function
gmf_r4_s3_pairs_range_inverse:
    r4_real_body 1,1,1
    .align 4
    .global gmf_r4_s3_pairs_publish_forward
    .type gmf_r4_s3_pairs_publish_forward,@function
gmf_r4_s3_pairs_publish_forward:
    r4_real_body 0,1,0,1
    .align 4
    .global gmf_r4_s3_pairs_publish_inverse
    .type gmf_r4_s3_pairs_publish_inverse,@function
gmf_r4_s3_pairs_publish_inverse:
    r4_real_body 1,1,0,1

/* Publish a wide real tile. A is4B aligned, B is the increasing16B-aligned
 * mirror block. Preserve the exact caller-selected bias/shift/saturation. */
    .align 4
    .global gmf_r4_s3_publish
    .type gmf_r4_s3_publish,@function
gmf_r4_s3_publish:
    entry sp,64
    s32i a6,sp,16
    addi a8,sp,16
    ee.vldbc.32 q6,a8
    ee.vld.128.ip q0,a4,16
    ee.vld.128.ip q1,a4,16
    ee.vld.128.ip q2,a4,16
    ee.vld.128.ip q3,a4,0
    ee.vadds.s32 q0,q0,q6
    ee.vadds.s32 q1,q1,q6
    ee.vadds.s32 q2,q2,q6
    ee.vadds.s32 q3,q3,q6
    ssr a5
    ee.vsr.32 q0,q0
    ee.vsr.32 q1,q1
    ee.vsr.32 q2,q2
    ee.vsr.32 q3,q3
    ee.orq q5,q3,q3
    r4_pack q0,q1
    r4_pack q2,q5
    .irp lane,0,1,2,3
        ee.movi.32.a q0,a8,\lane
        s32i a8,a2,(4*\lane)
        ee.movi.32.a q2,a8,(3-\lane)
        ee.movi.32.q q1,a8,\lane
    .endr
    ee.vst.128.ip q1,a3,0
    retw

/* Exact peak incl. -32768; vector max/min adapted from pinned GMF HP scan.
 * count>=8, multiple8. No out-of-bounds prefetch. */
    .align 4
    .global gmf_r4_s3_peak
    .type gmf_r4_s3_peak,@function
gmf_r4_s3_peak:
    entry sp,32
    srli a3,a3,3
    ee.zero.q q2
    ee.vld.128.ip q0,a2,16
    ee.orq q4,q0,q0
    ee.vsubs.s16 q1,q2,q0
    ee.vmax.s16 q0,q0,q1
    addi a3,a3,-1
    loopgtz a3,.Lr4_peak_end
        ee.vld.128.ip q1,a2,16
        ee.vmin.s16 q4,q4,q1
        ee.vsubs.s16 q3,q2,q1
        ee.vmax.s16 q1,q1,q3
        ee.vmax.s16 q0,q0,q1
.Lr4_peak_end:
    .irp amount,8,4,2
        movi a8,\amount
        wur.sar_byte a8
        ee.src.q q1,q0,q0
        ee.vmax.s16 q0,q0,q1
        ee.src.q q5,q4,q4
        ee.vmin.s16 q4,q4,q5
    .endr
    ee.movi.32.a q4,a8,0
    sext a8,a8,15
    movi a9,-32768
    beq a8,a9,.Lr4_peak_min
    ee.movi.32.a q0,a2,0
    extui a2,a2,0,16
    retw
.Lr4_peak_min:
    movi a2,32768
    retw
