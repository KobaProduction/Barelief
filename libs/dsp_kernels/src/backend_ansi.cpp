#include "backend.h"
#include "dsp_kernels/dsp.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
namespace dsp_kernels {
Backend compiled_backend() noexcept { return Backend::Ansi; }
namespace detail {
bool transform(float *scratch, std::size_t n, bool radix4) {
    if (radix4)
        return dsps_fft4r_fc32_ansi_(scratch, n, dsps_fft4r_w_table_fc32, dsps_fft4r_w_table_size) == ESP_OK &&
               dsps_bit_rev4r_fc32(scratch, n) == ESP_OK;
    return dsps_fft2r_fc32_ansi_(scratch, n, dsps_fft_w_table_fc32) == ESP_OK &&
           dsps_bit_rev_fc32_ansi(scratch, n) == ESP_OK;
}
}
}
