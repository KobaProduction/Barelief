/* Same vendor implementation and state utilities, with a separate Q15 ABI. */
#define FIXED_POINT
#define RANDOM_PREFIX qtvis_speex_q15
#define qtvis_speex_coefficient_bytes qtvis_speex_q15_coefficient_bytes
#define qtvis_speex_state_bytes qtvis_speex_q15_state_bytes
#define qtvis_speex_copy_state qtvis_speex_q15_copy_state
#define qtvis_speex_reset qtvis_speex_q15_reset
#define qtvis_speex_initial_position qtvis_speex_q15_initial_position
#define qtvis_speex_query qtvis_speex_q15_query
#define qtvis_speex_allocation_calls qtvis_speex_q15_allocation_calls
#include "speex_bridge.c"
