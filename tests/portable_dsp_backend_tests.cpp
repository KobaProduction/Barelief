#include "dsp_core/dsp.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include <cmath>
#include <iostream>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
int main() {
    try {
        // Dedicated process: no portable plan exists before direct vendor init,
        // and no table is deinitialized or replaced while plans are alive.
        for (bool radix4 : {false, true}) {
            require((radix4 ? dsps_fft4r_init_fc32(nullptr, 16) : dsps_fft2r_init_fc32(nullptr, 16)) == ESP_OK,
                    "External small-table initialization failed");
            dsp_core::MagnitudeSpectrum small, rejected;
            require(small.prepare(16, radix4), "Compatible external table rejected");
            const auto *table = radix4 ? dsps_fft4r_w_table_fc32 : dsps_fft_w_table_fc32;
            const auto table_size = radix4 ? dsps_fft4r_w_table_size : dsps_fft_w_table_size;
            require(!rejected.prepare(64, radix4), "Undersized external table accepted");
            require(table == (radix4 ? dsps_fft4r_w_table_fc32 : dsps_fft_w_table_fc32) &&
                        table_size == (radix4 ? dsps_fft4r_w_table_size : dsps_fft_w_table_size),
                    "Failed preparation replaced a live plan's table");
            std::vector<float> oversized(64, 1), oversized_output(32, -1);
            require(!rejected.process(oversized, oversized_output), "Failed preparation left a runnable plan");
            std::vector<float> input(16, 1), output(8);
            require(small.process(input, output) && std::abs(output[0] - 1) < 2e-5f,
                    "Failed preparation invalidated the smaller plan");
        }
        std::cout << "External FFT table ownership regressions passed\n";
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
