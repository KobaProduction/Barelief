/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Four butterflies, four contiguous complex values per stream. Every pointer
 * passed to the PIE batch is16-byte aligned. Tables retain pinned GMF layout.
 * No prepared plan mutation and no additional heap allocation are required. */
typedef struct {
    int16_t *stream[4];
    const int16_t *twiddle[3];
    int32_t product_shift; /*16 Q15,14+stage_shift HP*/
    int32_t bias_quarter;  /*0 Q15,2^(11+stage_shift) HP*/
} gmf_r4_batch;
int32_t gmf_r4_simd(int16_t*,const int16_t*,int32_t,int32_t,int,int);
void gmf_r4_s3_batch_forward(const gmf_r4_batch*);
void gmf_r4_s3_batch_inverse(const gmf_r4_batch*);
/* q=2: each stream is two complex values from two boxes32B apart. */
void gmf_r4_s3_small8_forward(const gmf_r4_batch*);
void gmf_r4_s3_small8_inverse(const gmf_r4_batch*);
/* Traverse disjoint boxes internally; private context copy, caller unchanged.
 * count>=1, stride in bytes; small8 count traverses pairs of length8 boxes. */
void gmf_r4_s3_boxes_forward(const gmf_r4_batch*,int32_t count,int32_t stride_bytes);
void gmf_r4_s3_boxes_inverse(const gmf_r4_batch*,int32_t count,int32_t stride_bytes);
void gmf_r4_s3_small8_boxes_forward(const gmf_r4_batch*,int32_t count,int32_t stride_bytes);
void gmf_r4_s3_small8_boxes_inverse(const gmf_r4_batch*,int32_t count,int32_t stride_bytes);
void gmf_r4_s3_unity(int16_t*,int32_t,int32_t,int32_t);
int32_t gmf_r4_s3_peak(const int16_t*,int32_t);
void gmf_r4_s3_pairs_forward(const int16_t*,const int16_t*,const int16_t*,int32_t*,int);
void gmf_r4_s3_pairs_inverse(const int16_t*,const int16_t*,const int16_t*,int32_t*,int);
/* Direct A starts at k=1 mod4 (4-byte alignment). B is the aligned increasing
 * block starting at m-k-3; the kernel reverses its complex values in registers. */
void gmf_r4_s3_pairs_direct_forward(const int16_t*,const int16_t*,const int16_t*,int32_t*,int);
void gmf_r4_s3_pairs_direct_inverse(const int16_t*,const int16_t*,const int16_t*,int32_t*,int);
/* Range-only first pass updates [min,max]; tile output is intentionally unused. */
void gmf_r4_s3_pairs_range_forward(const int16_t*,const int16_t*,const int16_t*,int32_t*,int,int32_t*);
void gmf_r4_s3_pairs_range_inverse(const int16_t*,const int16_t*,const int16_t*,int32_t*,int,int32_t*);
void gmf_r4_s3_publish(int16_t*,int16_t*,const int32_t*,int,int);
/* Fused direct pairs + exact quantization/publication, no wide output tile.
 * Arguments: A,B,W,product_bias/4,output_shift,output_bias. */
void gmf_r4_s3_pairs_publish_forward(int16_t*,int16_t*,const int16_t*,int,int,int);
void gmf_r4_s3_pairs_publish_inverse(int16_t*,int16_t*,const int16_t*,int,int,int);
void gmf_r4_real_pairs_model(const int16_t*,const int16_t*,const int16_t*,int32_t*,int,int);
int32_t gmf_hp4_pack_simd(int16_t*,const int16_t*,int32_t,int);
void gmf_q15_pack_simd(int16_t*,const int16_t*,int32_t,int);
/* Host semantic model, also the independent reference for firmware batches. */
void gmf_r4_batch_model(const gmf_r4_batch*,int inverse);
#ifdef __cplusplus
}
#endif
