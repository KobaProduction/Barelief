/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Internal S3 kernel:16B aligned, power-of-two complex count8..8192.
 * Pure permutation; log2_count must match count. */
void gmf_s3_bit_reverse_s16(int16_t* data,int32_t count,int32_t log2_count);
#ifdef __cplusplus
}
#endif
