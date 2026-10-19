/* SPDX-License-Identifier: Apache-2.0 */
#include "gmf_radix4_profile.h"
#if defined(GMF_R4_STAGE_PROFILE)
/* A single owned benchmark thread; not a production/global DSP profiler. */
static gmf_r4_profile* active;
void gmf_r4_profile_set(gmf_r4_profile* p){active=p;}
uint64_t gmf_r4_profile_now(void){return active?active->clock():0;}
void gmf_r4_profile_record(int kind,uint64_t start){if(active){active->ticks[kind]+=(uint32_t)active->clock()-(uint32_t)start;++active->calls[kind];}}
#endif
