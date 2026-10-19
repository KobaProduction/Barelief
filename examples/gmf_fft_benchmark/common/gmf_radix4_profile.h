/* SPDX-License-Identifier: Apache-2.0
 * Optional benchmark instrumentation, compiled out of performance images. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
enum {GMF_R4_COMPLEX,GMF_R4_PEAK,GMF_R4_BATCH,GMF_R4_TAIL,GMF_R4_REAL,GMF_R4_PAIRS,GMF_R4_PERMUTATION,GMF_R4_PROFILE_COUNT};
/* Clock returns a modulo32 cycle counter. Each interval must be shorter than
 * one wrap; modular subtraction also handles a wrap during a transform. */
typedef struct {uint64_t (*clock)(void);uint64_t ticks[GMF_R4_PROFILE_COUNT];uint32_t calls[GMF_R4_PROFILE_COUNT];} gmf_r4_profile;
#if defined(GMF_R4_STAGE_PROFILE)
void gmf_r4_profile_set(gmf_r4_profile*);
uint64_t gmf_r4_profile_now(void);
void gmf_r4_profile_record(int,uint64_t);
#define GMF_R4_PROFILE_BEGIN(name) uint64_t name=gmf_r4_profile_now()
#define GMF_R4_PROFILE_END(name,kind) gmf_r4_profile_record(kind,name)
#else
#define GMF_R4_PROFILE_BEGIN(name) ((void)0)
#define GMF_R4_PROFILE_END(name,kind) ((void)0)
#endif
#ifdef __cplusplus
}
#endif
