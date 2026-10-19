/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
/* One complex value, without a ROM memcpy call or stronger alignment/aliasing
 * assumptions. Reading both halves first also preserves memcpy ordering. */
#if defined(__GNUC__)
#define GMF_COPY_INLINE static inline __attribute__((always_inline))
#else
#define GMF_COPY_INLINE static inline
#endif
GMF_COPY_INLINE void gmf_copy_complex(int16_t* dst,const int16_t* src) {
    const int16_t re=src[0],im=src[1];dst[0]=re;dst[1]=im;
}
#undef GMF_COPY_INLINE
