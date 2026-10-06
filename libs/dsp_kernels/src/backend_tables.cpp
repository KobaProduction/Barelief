#include "backend.h"
#include "dsp_kernels/dsp.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include <mutex>
namespace dsp_kernels::detail {
bool prepare_backend(std::size_t n, bool radix4) {
    // Serialize initialization only. Direct SDK init/deinit must not run
    // concurrently, and deinit must not run while any prepared plan lives.
    static std::mutex init_mutex;
    std::lock_guard lock(init_mutex);
    const auto status = radix4 ? dsps_fft4r_init_fc32(nullptr, max_radix4_samples)
                              : dsps_fft2r_init_fc32(nullptr, max_fft_samples);
    if (status != ESP_OK) return false;
    // Preserve external ownership: no resize or deinit of existing tables.
    return radix4 ? dsps_fft4r_w_table_fc32 && dsps_fft4r_w_table_size >= 2 * n
                  : dsps_fft_w_table_fc32 && dsps_fft_w_table_size >= n;
}
std::size_t shared_table_bytes(bool radix4) {
    std::size_t log2 = 0, log4 = 0;
    for (auto n = max_fft_samples; n > 1; n >>= 1) ++log2;
    for (auto n = max_radix4_samples; n > 1; n >>= 2) ++log4;
    return radix4 ? max_radix4_samples * 4 * sizeof(float) +
                        2 * dsps_fft4r_rev_tables_fc32_size[log4 - 2] * sizeof(uint16_t)
                  : max_fft_samples * sizeof(float) +
                        2 * dsps_fft2r_rev_tables_fc32_size[log2 - 4] * sizeof(uint16_t);
}
}
