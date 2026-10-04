#include "dsp_core/dsp.h"
#include "dsps_fft2r.h"
#include "dsps_fft4r.h"
#include "dsps_wind_hann.h"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <numbers>
#include <stdexcept>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void near(float actual, float expected, const char *message) {
    require(std::isfinite(actual) && std::abs(actual - expected) < 2e-5f * std::max(1.0f, std::abs(expected)), message);
}
int main() {
    try {
        // A power of four that fits every supported build profile.
        constexpr auto n = dsp_core::max_radix4_samples;
        constexpr auto tone_bin = n / 16;
        std::vector<float> wave(n), window(n), output(n / 2), legacy(n / 2), scratch(n * 2);
        dsps_wind_hann_f32(window.data(), n);
        float max_weighted_error = 0;
        for (bool radix4 : {false, true}) {
            dsp_core::FFT fft;
            require(fft.prepare(n, radix4), "FFT preparation failed");
            auto run_legacy = [&] {
                for (std::size_t i = 0; i < n; ++i) {
                    scratch[2 * i] = wave[i];
                    scratch[2 * i + 1] = 0;
                }
                // Exact FFT2R/FFT4R sequence from HEAD's analyzer/analyze/fft.cpp,
                // including the real-spectrum conversion before magnitude.
                if (radix4) {
                    require(dsps_fft4r_fc32_ansi_(scratch.data(), n, dsps_fft4r_w_table_fc32,
                                                  dsps_fft4r_w_table_size) == ESP_OK &&
                                dsps_bit_rev4r_fc32(scratch.data(), n) == ESP_OK &&
                                dsps_cplx2real_fc32_ansi_(scratch.data(), n, dsps_fft4r_w_table_fc32,
                                                        dsps_fft4r_w_table_size) == ESP_OK,
                            "Legacy FFT4R failed");
                } else {
                    require(dsps_fft2r_fc32_ansi_(scratch.data(), n, dsps_fft_w_table_fc32) == ESP_OK &&
                                dsps_bit_rev_fc32_ansi(scratch.data(), n) == ESP_OK &&
                                dsps_cplx2reC_fc32_ansi(scratch.data(), n) == ESP_OK,
                            "Legacy FFT2R failed");
                }
                for (std::size_t i = 0; i < n / 2; ++i)
                    legacy[i] = std::sqrt(std::pow(scratch[2 * i], 2.0f) +
                                          std::pow(scratch[2 * i + 1], 2.0f)) / n;
            };
            for (int case_id = 0; case_id < 6; ++case_id) {
                for (std::size_t i = 0; i < n; ++i) {
                    const float value = case_id == 0 ? 1.0f
                        : case_id == 1 ? (i == 100 ? 1.0f : 0.0f)
                        : case_id == 2 ? std::sin(2 * std::numbers::pi * tone_bin * i / n)
                        : case_id == 3 ? std::sin(2 * std::numbers::pi * (tone_bin - 0.3) * i / n) +
                                         0.21f * std::cos(i * 0.773)
                        : case_id == 4 ? 1.0f
                                       : std::sin(2 * std::numbers::pi * tone_bin * i / n);
                    wave[i] = value * (case_id < 4 ? window[i] : 1.0f);
                }
                run_legacy();
                require(fft.process(wave, output), "Portable FFT failed");
                for (std::size_t k = 0; k < output.size(); ++k) {
                    // Intentional mathematical corrections, not regressions:
                    // old radix2 doubled every non-DC bin; old radix4 placed DC
                    // in both real and imaginary slots, making magnitude sqrt(2)*DC.
                    const auto expected_old = output[k] *
                        (radix4 ? (k == 0 ? std::sqrt(2.0f) : 1.0f) : (k == 0 ? 1.0f : 2.0f));
                    near(legacy[k], expected_old, "Legacy normalization differs from documented correction");
                    if (radix4) {
                        const float base = float(k) * (48000.0f / n / 650.0f);
                        const float bark = 7 * std::log(base + std::sqrt(1 + base * base));
                        max_weighted_error = std::max(max_weighted_error, std::abs(legacy[k] - output[k]) * bark);
                    }
                }
                // Independent DFT probes prevent two backend routes from agreeing
                // on an incorrect result. DC and tone cases also pin absolute scale.
                for (auto k : {std::size_t{0}, std::size_t{1}, tone_bin, n / 2 - 1}) {
                    double re = 0, im = 0;
                    for (std::size_t i = 0; i < n; ++i) {
                        const auto angle = -2 * std::numbers::pi * k * i / n;
                        re += wave[i] * std::cos(angle);
                        im += wave[i] * std::sin(angle);
                    }
                    near(output[k], std::hypot(re, im) / n, "Portable FFT differs from independent DFT");
                }
                if (case_id == 4) {
                    near(output[0], 1, "Corrected DC magnitude is not one");
                    near(legacy[0], radix4 ? std::sqrt(2.0f) : 1.0f, "Old DC scale not reproduced");
                }
                if (case_id == 5) {
                    near(output[tone_bin], 0.5f, "Corrected sinusoid magnitude is not one half");
                    near(legacy[tone_bin], radix4 ? 0.5f : 1.0f, "Old sinusoid scale not reproduced");
                }
            }
        }
        require(max_weighted_error < 2e-5f, "Desktop FFT4R Hann/Bark spectrum changed");
        std::cout << "Exact legacy FFT2R/FFT4R routes and corrected DFT normalization passed; "
                     "max FFT4R Bark-weighted error " << max_weighted_error << '\n';
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
