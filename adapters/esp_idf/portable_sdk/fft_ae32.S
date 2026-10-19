// Shared ESP32 LX6 / ESP32-S3 LX7 scalar Float32 FPU kernels.
// Hardware loops and interleaved independent FPU operations follow the
// instruction techniques of ESP-DSP ae32; the formulas match scalar RFFT.
    .section .iram1,"ax"
    .align 4
    .global dsp_fft_input_max_ae32
    .type dsp_fft_input_max_ae32,@function
dsp_fft_input_max_ae32:
    entry a1,16
    movi.n a5,0
    srli a11,a3,2
    movi.n a6,-1
    srli a6,a6,1
    loopnez a11,.max_words_end
    l32i.n a7,a2,0
    l32i.n a8,a2,4
    l32i a9,a2,8
    l32i a10,a2,12
    and a7,a7,a6
    and a8,a8,a6
    and a9,a9,a6
    and a10,a10,a6
    maxu a5,a5,a7
    maxu a5,a5,a8
    maxu a5,a5,a9
    maxu a5,a5,a10
    addi a2,a2,16
.max_words_end:
    extui a3,a3,0,2
    loopnez a3,.max_tail_end
    l32i.n a7,a2,0
    and a7,a7,a6
    maxu a5,a5,a7
    addi.n a2,a2,4
.max_tail_end:
    movi a6,255
    slli a6,a6,23
    bgeu a5,a6,.max_bad
    s32i.n a5,a4,0
    movi.n a2,1
    retw.n
.max_bad:
    movi.n a2,0
    retw.n

    .align 4
    .global dsp_fft_all_finite_ae32
    .type dsp_fft_all_finite_ae32,@function
dsp_fft_all_finite_ae32:
    entry a1,16
    movi.n a5,0
    srli a6,a3,2
    loopnez a6,.finite_words_end
    l32i.n a7,a2,0
    l32i.n a8,a2,4
    l32i a9,a2,8
    l32i a10,a2,12
    extui a7,a7,23,8
    extui a8,a8,23,8
    extui a9,a9,23,8
    extui a10,a10,23,8
    maxu a5,a5,a7
    maxu a5,a5,a8
    maxu a5,a5,a9
    maxu a5,a5,a10
    addi a2,a2,16
.finite_words_end:
    extui a3,a3,0,2
    loopnez a3,.finite_tail_end
    l32i.n a7,a2,0
    extui a7,a7,23,8
    maxu a5,a5,a7
    addi.n a2,a2,4
.finite_tail_end:
    movi a6,255
    beq a5,a6,.finite_bad
    movi.n a2,1
    retw.n
.finite_bad:
    movi.n a2,0
    retw.n

    .align 4
    .global dsp_rfft_pack_ae32
    .type dsp_rfft_pack_ae32,@function
// input a2, even/odd packed output a3, complex half-size a4.
dsp_rfft_pack_ae32:
    entry a1,16
    addx8 a5,a4,a3
    loopnez a4,.pack_end
    l32i.n a6,a2,0
    l32i.n a7,a2,4
    l32i a8,a2,8
    l32i a9,a2,12
    s32i.n a6,a3,0
    s32i.n a7,a3,4
    s32i.n a8,a5,0
    s32i.n a9,a5,4
    addi a2,a2,16
    addi.n a3,a3,8
    addi.n a5,a5,8
.pack_end:
    retw.n

    .macro mixed_pair step,negative
    lsi f0,a3,0
    lsi f2,a7,0
    lsi f4,a4,0
    lsi f3,a7,4
    .if \negative
    neg.s f4,f4
    .endif
    mul.s f6,f4,f2
    lsi f5,a4,4
    mul.s f7,f4,f3
    lsi f1,a3,4
    msub.s f6,f5,f3
    madd.s f7,f5,f2
    add.s f8,f0,f6
    sub.s f10,f0,f6
    add.s f9,f1,f7
    sub.s f11,f1,f7
    ssi f8,a2,0
    ssi f10,a6,0
    ssi f9,a2,4
    ssi f11,a6,4
    addi.n a2,a2,8
    addi.n a6,a6,8
    addi.n a3,a3,8
    addi.n a7,a7,8
    addi a4,a4,\step
    .endm
    .align 4
    .global dsp_rfft_mixed_ae32
    .type dsp_rfft_mixed_ae32,@function
// output a2, two FFT halves a3, quadrant twiddles a4, half-size a5.
dsp_rfft_mixed_ae32:
    entry a1,16
    addx8 a6,a5,a2
    addx8 a7,a5,a3
    srli a9,a5,1
    addi.n a8,a9,1
    addi.n a9,a9,-1
    loopnez a8,.mixed_positive_end
    mixed_pair 16,0
.mixed_positive_end:
    addi a4,a4,-32
    loopnez a9,.mixed_negative_end
    mixed_pair -16,1
.mixed_negative_end:
    retw.n

    .align 4
    .global dsp_rfft_split_ae32
    .type dsp_rfft_split_ae32,@function
// data a2, quadrant twiddles a3, packed complex size a4.
dsp_rfft_split_ae32:
    entry a1,16
    // Self-conjugate midpoint: X = {Re(Z), -Im(Z)}.
    addx4 a6,a4,a2
    lsi f0,a6,4
    neg.s f0,f0
    ssi f0,a6,4
    addi.n a5,a4,-1
    addx8 a5,a5,a2
    srli a4,a4,1
    addi.n a4,a4,-1
    addi.n a2,a2,8
    addi.n a3,a3,8
    movi a7,63
    slli a7,a7,24
    wfr f15,a7
    loopnez a4,.split_end
#if defined(DSP_FFT_AES3)
    // Internal buffers hold aligned complex pairs; arithmetic stays identical.
    ee.ldf.64.ip f1,f0,a2,0
    ee.ldf.64.ip f3,f2,a5,0
#else
    lsi f0,a2,0
    lsi f2,a5,0
    lsi f1,a2,4
    lsi f3,a5,4
#endif
    add.s f6,f0,f2
    sub.s f9,f2,f0
    sub.s f7,f1,f3
    add.s f8,f1,f3
    mul.s f6,f6,f15
    mul.s f9,f9,f15
    mul.s f7,f7,f15
    mul.s f8,f8,f15
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f5,f4,a3,8
#else
    lsi f4,a3,0
    lsi f5,a3,4
#endif
    mul.s f10,f4,f8
    mul.s f11,f4,f9
    msub.s f10,f5,f9
    madd.s f11,f5,f8
    add.s f0,f6,f10
    sub.s f2,f6,f10
    add.s f1,f7,f11
    sub.s f3,f11,f7
#if defined(DSP_FFT_AES3)
    ee.stf.64.ip f1,f0,a2,8
    ee.stf.64.ip f3,f2,a5,-8
#else
    ssi f0,a2,0
    ssi f2,a5,0
    ssi f1,a2,4
    ssi f3,a5,4
    addi.n a2,a2,8
    addi a5,a5,-8
    addi.n a3,a3,8
#endif
.split_end:
    retw.n
// Three-rotation final real radix4 stage. Interior groups k,h-k,2h-k,h+k;
// DC, Nyquist, h and self-paired h/2 are handled by the portable wrapper.
    .section .iram1.dsp_rfft_fused,"ax"
    .align 4
    .macro fused_group step,negative
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f1,f0,a3,8
    ee.ldf.64.ip f3,f2,a9,-8
#else
    lsi f1,a3,4
    lsi f3,a9,4
    lsip f0,a3,8
    lsi f2,a9,0
    addi a9,a9,-8
#endif
    add.s f8,f0,f2
    sub.s f11,f2,f0
    sub.s f9,f1,f3
    add.s f10,f1,f3
    mul.s f8,f8,f15
    mul.s f9,f9,f15
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f5,f4,a10,8
    ee.ldf.64.ip f7,f6,a11,-8
#else
    lsi f5,a10,4
    lsi f7,a11,4
    lsip f4,a10,8
    lsi f6,a11,0
    addi a11,a11,-8
#endif
    add.s f0,f4,f6
    sub.s f3,f6,f4
    sub.s f1,f5,f7
    add.s f2,f5,f7
    // R+iI = W^(2k) * (U+iV)
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f5,f4,a12,16
#else
    lsi f5,a12,4
    lsip f4,a12,16
#endif
    mul.s f6,f4,f0
    mul.s f7,f4,f1
    msub.s f6,f5,f1
    madd.s f7,f5,f0
    // L+iN = W^k * (C+iD)
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f5,f4,a4,8
#else
    lsi f5,a4,4
    lsip f4,a4,8
#endif
    mul.s f0,f4,f10
    mul.s f1,f4,f11
    msub.s f0,f5,f11
    madd.s f1,f5,f10
    // M+iO = W^(3k) * (S+iT), quadrant reflection after 3k>h.
#if defined(DSP_FFT_AES3)
    ee.ldf.64.ip f5,f4,a13,\step
#else
    lsi f5,a13,4
    .if \negative
    lsi f4,a13,0
    addi a13,a13,\step
    .else
    lsip f4,a13,\step
    .endif
#endif
    .if \negative
    neg.s f4,f4
    .endif
    mul.s f10,f4,f2
    mul.s f11,f4,f3
    msub.s f10,f5,f3
    madd.s f11,f5,f2
    add.s f2,f8,f6
    sub.s f3,f8,f6
    add.s f4,f9,f7
    sub.s f5,f7,f9
    add.s f6,f0,f10
    add.s f7,f1,f11
    sub.s f12,f11,f1
    sub.s f13,f10,f0
    add.s f8,f2,f6
    add.s f9,f4,f7
    sub.s f10,f2,f6
    sub.s f11,f7,f4
    add.s f0,f3,f12
    add.s f1,f5,f13
    sub.s f2,f3,f12
    sub.s f3,f13,f5
#if defined(DSP_FFT_AES3)
    ee.stf.64.ip f9,f8,a2,8
    ee.stf.64.ip f11,f10,a7,-8
    ee.stf.64.ip f1,f0,a6,-8
    ee.stf.64.ip f3,f2,a8,8
#else
    ssi f9,a2,4
    ssip f8,a2,8
    ssi f11,a7,4
    ssi f10,a7,0
    addi a7,a7,-8
    ssi f1,a6,4
    ssi f0,a6,0
    addi a6,a6,-8
    ssi f3,a8,4
    ssip f2,a8,8
#endif
    .endm
    .global dsp_rfft_fused_pairs_ae32
    .type dsp_rfft_fused_pairs_ae32,@function
// output a2, two complex FFT(h) outputs a3, quadrant twiddles a4, h a5.
dsp_rfft_fused_pairs_ae32:
    entry a1,32
    s32i.n a4,a1,0
    s32i.n a5,a1,4
    srli a6,a5,1
    addi.n a15,a6,-1
    movi.n a6,3
    quou a14,a5,a6
    sub a15,a15,a14
    addi.n a6,a5,-1
    addx8 a9,a6,a3
    addx8 a6,a6,a2
    addx8 a10,a5,a3
    addi.n a11,a5,-1
    addx8 a11,a11,a10
    addi.n a10,a10,8
    slli a7,a5,1
    addi.n a7,a7,-1
    addx8 a7,a7,a2
    addi.n a8,a5,1
    addx8 a8,a8,a2
    addi.n a3,a3,8
    addi.n a2,a2,8
    addi a12,a4,16
    addi a13,a4,24
    addi.n a4,a4,8
    movi a5,63
    slli a5,a5,24
    wfr f15,a5
    loopnez a14,.fused_positive_end
    fused_group 24,0
.fused_positive_end:
    // Reflect next 3k index around h: address = 2*(w+h*8)-address.
    l32i.n a14,a1,0
    l32i.n a5,a1,4
    addx8 a14,a5,a14
    add a14,a14,a14
    sub a13,a14,a13
    loopnez a15,.fused_negative_end
    fused_group -24,1
.fused_negative_end:
    retw.n
    .size dsp_rfft_fused_pairs_ae32,.-dsp_rfft_fused_pairs_ae32
// Classify and copy contiguous real pairs in one pass, preserving all bits.
#if defined(DSP_FFT_EXTRA)
// n is a multiple of four; the small-size tail also covers N=8.
    .section .iram1.dsp_rfft_copymax,"ax"
    .align 4
    .macro copymax_quad offset
    l32i a9,a2,\offset
    l32i a10,a2,\offset+4
    l32i a11,a2,\offset+8
    l32i a12,a2,\offset+12
    s32i a9,a3,\offset
    s32i a10,a3,\offset+4
    s32i a11,a3,\offset+8
    s32i a12,a3,\offset+12
    and a9,a9,a8
    and a10,a10,a8
    and a11,a11,a8
    and a12,a12,a8
    maxu a7,a7,a9
    maxu a7,a7,a10
    maxu a7,a7,a11
    maxu a7,a7,a12
    .endm
    .global dsp_rfft_copy_max_ae32
    .type dsp_rfft_copy_max_ae32,@function
// input a2, private destination a3, n a4, maximum output a5.
dsp_rfft_copy_max_ae32:
    entry a1,16
    srli a6,a4,4
    extui a4,a4,0,4
    srli a4,a4,2
    movi.n a7,0
    movi.n a8,-1
    srli a8,a8,1
    loopnez a6,.copymax_bulk_end
    copymax_quad 0
    copymax_quad 16
    copymax_quad 32
    copymax_quad 48
    addi a2,a2,64
    addi a3,a3,64
.copymax_bulk_end:
    loopnez a4,.copymax_tail_end
    copymax_quad 0
    addi a2,a2,16
    addi a3,a3,16
.copymax_tail_end:
    movi a8,255
    slli a8,a8,23
    bgeu a7,a8,.copymax_bad
    s32i.n a7,a5,0
    movi.n a2,1
    retw.n
.copymax_bad:
    movi.n a2,0
    retw.n
    .size dsp_rfft_copy_max_ae32,.-dsp_rfft_copy_max_ae32
#endif
// Classify and pack in the same pass; h=N/4 is a power of four, h>=4.
    .section .iram1.dsp_rfft_packmax,"ax"
    .align 4
    .macro packmax_quad input_offset,output_offset
    l32i a9,a2,\input_offset
    l32i a10,a2,\input_offset+4
    l32i a11,a2,\input_offset+8
    l32i a12,a2,\input_offset+12
    s32i a9,a3,\output_offset
    s32i a10,a3,\output_offset+4
    s32i a11,a6,\output_offset
    s32i a12,a6,\output_offset+4
    and a9,a9,a8
    and a10,a10,a8
    and a11,a11,a8
    and a12,a12,a8
    maxu a7,a7,a9
    maxu a7,a7,a10
    maxu a7,a7,a11
    maxu a7,a7,a12
    .endm
    .global dsp_rfft_pack_max_ae32
    .type dsp_rfft_pack_max_ae32,@function
dsp_rfft_pack_max_ae32:
    entry a1,16
    addx8 a6,a4,a3
    movi.n a7,0
    movi.n a8,-1
    srli a8,a8,1
    srli a4,a4,2
    loopnez a4,.packmax_end
    packmax_quad 0,0
    packmax_quad 16,8
    packmax_quad 32,16
    packmax_quad 48,24
    addi a2,a2,64
    addi a3,a3,32
    addi a6,a6,32
.packmax_end:
    movi a8,255
    slli a8,a8,23
    bgeu a7,a8,.packmax_bad
    s32i.n a7,a5,0
    movi.n a2,1
    retw.n
.packmax_bad:
    movi.n a2,0
    retw.n
    .size dsp_rfft_pack_max_ae32,.-dsp_rfft_pack_max_ae32
