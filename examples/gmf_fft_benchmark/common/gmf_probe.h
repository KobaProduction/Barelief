#pragma once
#include <stddef.h>
#include "esp_gmf_fft.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Experimental observability only, tied to the pinned upstream translation unit.
 * No arithmetic or upstream buffer layout is changed. Not a production API. */
int gmf_probe_exponent(esp_gmf_fft_handle_t handle);
size_t gmf_probe_plan_bytes(void);
size_t gmf_probe_live_bytes(void);
size_t gmf_probe_allocations(void);
int gmf_probe_guards_ok(void);
#ifdef __cplusplus
}
#endif
