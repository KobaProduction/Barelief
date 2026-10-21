#include "gmf_optimized.h"
#include "gmf_probe.h"
#include "esp_gmf_fft_heap.h"
#include "test_oracle.h"
#include <cstdio>
#include <cstring>
#include <cmath>
#include <utility>
namespace {
constexpr double tau = 6.2831853071795864769;
struct HpCheck {
    size_t n;
    esp_gmf_fft_handle_t plan = nullptr;
    int16_t *a = nullptr, *b = nullptr, *c = nullptr;
    std::vector<int16_t> input;
    uint32_t rng = 0x1949;
    explicit HpCheck(size_t size) : n(size) {}
    bool initialize() {
        if (gmf_optimized_init(n, &plan) != ESP_GMF_FFT_OK)
            return false;
        a = (int16_t *)esp_gmf_fft_calloc_aligned(n + 2, 2, 16);
        b = (int16_t *)esp_gmf_fft_calloc_aligned(n + 2, 2, 16);
        c = (int16_t *)esp_gmf_fft_calloc_aligned(n + 2, 2, 16);
        if (!a || !b || !c)
            return false;
        input.resize(n);
        return true;
    }
    int16_t tonal_fixture_value(unsigned test, size_t i, int v) const {
        if (test == 4)
            return int16_t(int(4096 * std::cos(tau * 13 * i / n + .37) + 2000 * std::cos(tau * 71 * i / n + .81)));
        if (test == 5)
            return int16_t(v / 4);
        if (test == 6)
            return int16_t(int(4096 * std::cos(tau * 13 * i / n + .37) + .4096 * std::cos(tau * 71 * i / n + .81)));
        return int16_t(v);
    }
    int16_t fixture_value(unsigned test, size_t i, int v) const {
        switch (test) {
        case 0: return 0;
        case 1: return i == 17 ? 24576 : 0;
        case 2: return 4096;
        case 3: return (i & 1) ? -4096 : 4096;
        case 7: return (i & 1) ? -32768 : 32767;
        default: return tonal_fixture_value(test, i, v);
        }
    }
    void fill(unsigned test) {
        for (size_t i = 0; i < n; ++i) {
            rng = rng * 1664525u + 1013904223u;
            int v = int(rng >> 16) - 32768;
            input[i] = fixture_value(test, i, v);
        }
    }
    bool compare_large_stock() {
        gmf_optimized_forward_hp(plan, b, 0);
        const double scale = std::ldexp(1., gmf_probe_exponent(plan) + 15);
        auto ref = gmf_test::reference(input);
        double p = 0, e = 0;
        for (size_t k = 0; k <= n / 2; ++k) {
            auto value = k == 0       ? std::complex<double>(b[0], 0)
                         : k == n / 2 ? std::complex<double>(b[1], 0)
                                      : std::complex<double>(b[2 * k], b[2 * k + 1]);
            p += std::norm(ref[k]);
            e += std::norm(value * scale - ref[k]);
        }
        if (p && e && 10 * std::log10(p / e) < 45)
            return false;
        return true;
    }
    bool compare_stock(unsigned test) {
        if (n <= 8192) {
            esp_gmf_fft_forward_hp(plan, a);
            const int exp = gmf_probe_exponent(plan);
            gmf_optimized_forward_hp(plan, b, 0);
            if (exp != gmf_probe_exponent(plan) || std::memcmp(a, b, (n + 2) * 2))
                return false;
            esp_gmf_fft_inverse_hp(plan, a);
            gmf_optimized_inverse_hp(plan, b, 0);
            if (std::memcmp(a, b, n * 2))
                return false;
        } else if (test < 7)
            return compare_large_stock();
        return true;
    }
    std::pair<double, double> forward_error(int exponent) const {
        auto ref = gmf_test::reference(input);
        double p = 0, e = 0;
        const double scale = std::ldexp(1., exponent + 15);
        for (size_t k = 0; k <= n / 2; ++k) {
            auto value = k == 0       ? std::complex<double>(c[0], 0)
                         : k == n / 2 ? std::complex<double>(c[1], 0)
                                      : std::complex<double>(c[2 * k], c[2 * k + 1]);
            const double w = k == 0 || k == n / 2 ? 1 : 2;
            p += w * std::norm(ref[k]);
            e += w * std::norm(value * scale - ref[k]);
        }
        return {p, e};
    }
    bool forward(unsigned test) {
        gmf_optimized_forward_hp(plan, c, 1);
        const int exponent = gmf_probe_exponent(plan);
        if (test < 8) {
            const auto [p, e] = forward_error(exponent);
            const double snr = e == 0 ? 300 : p == 0 ? -300 : 10 * std::log10(p / e);
            std::printf("hp4 N=%u fixture=%u forward_snr_db=%.6f exponent=%d\n", unsigned(n), test, snr, exponent);
            if (p && snr < 45)
                return false;
        }
        return true;
    }
    bool inverse(unsigned test) {
        gmf_optimized_inverse_hp(plan, c, 1);
        if (test < 8) {
            double p = 0, e = 0;
            for (size_t i = 0; i < n; ++i) {
                p += double(input[i]) * input[i];
                double d = double(c[i]) - input[i];
                e += d * d;
            }
            const double snr = e == 0 ? 300 : p == 0 ? -300 : 10 * std::log10(p / e);
            std::printf("hp4 N=%u fixture=%u roundtrip_snr_db=%.6f\n", unsigned(n), test, snr);
            if (p && snr < 40)
                return false;
        }
        if (!gmf_probe_guards_ok())
            return false;
        return true;
    }
    bool fixtures() {
        for (unsigned test = 0; test < 48; ++test) {
            fill(test);
            std::memcpy(a, input.data(), n * 2);
            std::memcpy(b, a, n * 2);
            std::memcpy(c, a, n * 2);
            if (!compare_stock(test) || !forward(test) || !inverse(test))
                return false;
        }
        return true;
    }
    bool verify_inverse_spectrum(const std::vector<int16_t> &spectrum) const {
        for (size_t t : {size_t(0), size_t(1), size_t(17), n / 4, n / 2, n - 1}) {
            double ref = spectrum[0] + double(spectrum[1]) * (t % 2 ? -1 : 1);
            for (size_t k = 1; k < n / 2; ++k) {
                double angle = tau * k * t / n;
                ref += 2 * (spectrum[2 * k] * std::cos(angle) - spectrum[2 * k + 1] * std::sin(angle));
            }
            if (std::abs(c[t] - ref / n) > 3)
                return false;
        }
        return true;
    }
    bool independent_inverse() {
        std::memset(c, 0, (n + 2) * 2);
        gmf_optimized_forward_hp(plan, c, 1);
        std::vector<int16_t> spectrum(n + 2);
        for (auto &v : spectrum) {
            rng = rng * 1664525u + 1013904223u;
            v = int16_t(int(rng >> 24) - 128);
        }
        spectrum[n] = spectrum[1];
        spectrum[n + 1] = 0;
        std::memcpy(c, spectrum.data(), (n + 2) * 2);
        gmf_optimized_inverse_hp(plan, c, 1);
        return verify_inverse_spectrum(spectrum);
    }
    bool finish() {
        esp_gmf_fft_free_aligned(a);
        esp_gmf_fft_free_aligned(b);
        esp_gmf_fft_free_aligned(c);
        esp_gmf_fft_deinit(&plan);
        std::printf("hp_candidate N=%u tests=48 r2_stock_bit_exact=%s independent_inverse=PASS guards=PASS\n",
                    unsigned(n), n <= 8192 ? "PASS" : "unsupported");
        return gmf_probe_live_bytes() == 0;
    }
};
bool check(size_t n) {
    HpCheck scenario(n);
    return scenario.initialize() && scenario.fixtures() && scenario.independent_inverse() && scenario.finish();
}
} // namespace
int main() {
    for (size_t n : {512, 1024, 2048, 4096, 8192, 16384})
        if (!check(n)) {
            std::printf("hp_candidate result=FAIL N=%u\n", unsigned(n));
            return 1;
        }
    std::printf("hp_candidate result=PASS\n");
    return 0;
}
