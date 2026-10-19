#define CONFIG_DSP_MAX_FFT_SIZE PORTABLE_DSP_MAX_FFT_SIZE
// Compile the unchanged vendor initializer with deterministic allocation faults.
// The numerical kernels and production backend are linked normally.
#include <stdlib.h>
#include <malloc.h>
void *fft_fault_malloc(size_t size);
void fft_fault_free(void *pointer);
#define malloc fft_fault_malloc
#define free fft_fault_free
#include "../libs/dsp_kernels/vendor/esp_dsp/fft/float/dsps_fft4r_fc32_ansi.c"
