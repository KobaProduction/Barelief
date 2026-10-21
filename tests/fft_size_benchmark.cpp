#include "dsp_kernels/dsp.h"
#include "rfft_numerical_oracle.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#if defined(_WIN32)
#include <malloc.h>
#endif
#include <new>
#include <numbers>
#include <stdexcept>
#include <vector>
namespace {
bool watching=false;
unsigned allocations=0;
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
void near(double a,double b,double tolerance) {
    check(std::isfinite(a) && std::abs(a-b)<=tolerance+5e-6*std::abs(b), "Numerical comparison failed");
}
struct SizeBenchmark {
    static constexpr std::size_t frames = 8, routes = 6, batches = 160, batch = 32;
    std::size_t n;
    std::array<std::vector<float>, frames> input, reference;
    std::array<std::vector<float>, routes> output;
    std::array<std::array<double, batches>, routes> elapsed{};
    std::array<double, routes> errors{};
    std::array<dsp_kernels::FFT, 2> full;
    std::array<dsp_kernels::PreparedRfft, 4> real;
    bool full4;
    std::array<bool, routes> active;
    double tolerance, oracle_error = 0;
    explicit SizeBenchmark(std::size_t size)
        : n(size), full4((n & 0x55555555) != 0), active{true, full4, true, true, true, true},
          tolerance(.0003 * std::max(1., double(n) / 4096.)) {}
    void fill_input(const std::vector<float> &coefficients) {
        constexpr std::array<double, 7> tones{3, 11.25, 23, 41.5, 67, 89.75, 111};
        for (std::size_t f = 0; f < frames; ++f) {
            input[f].resize(n);
            reference[f].resize(n + 2);
            for (std::size_t i = 0; i < n; ++i) {
                double value = .17 + (i % 2 ? -.11 : .11);
                for (std::size_t t = 0; t < tones.size(); ++t)
                    value += (.25 / double(t + 1)) *
                             std::cos(2 * std::numbers::pi * tones[t] * double(i) / double(n) + .13 * t + .19 * f);
                input[f][i] = float(value) * coefficients[i];
            }
        }
    }
    void prepare() {
        using namespace dsp_kernels;
        std::vector<float> coefficients(n);
        window(coefficients, WindowFunction::Hann);
        fill_input(coefficients);
        for (auto &values : output)
            values.resize(n + 2);
        check(full[0].prepare(n, false), "CFFT2 prepare failed");
        check(full[1].prepare(n, true) == full4, "CFFT4 size contract failed");
        for (std::size_t r = 0; r < 4; ++r)
            check(real[r].prepare(n, r >= 2, r % 2 ? PreparedRfft::Split::Automatic : PreparedRfft::Split::Scalar),
                  "RFFT prepare failed");
        for (std::size_t f = 0; f < frames; ++f)
            check(full[0].process(input[f], reference[f]), "Reference FFT failed");
    }
    void verify_reference() {
        // Reuse the independent direct time-domain double DFT at the same bins.
        const std::array<std::size_t, 16> probes{0,  1,  3,  11,  12,    23,    41,        42,
                                                 67, 89, 90, 111, n / 8, n / 4, n / 2 - 1, n / 2};
        std::vector<double> expected(n + 2);
        for (std::size_t f = 0; f < frames; ++f)
            for (auto k : probes) {
                rfft_test_oracle::direct_bin(input[f], expected, k);
                near(reference[f][2 * k], expected[2 * k], tolerance);
                near(reference[f][2 * k + 1], expected[2 * k + 1], tolerance);
                oracle_error = std::max({oracle_error, std::abs(reference[f][2 * k] - expected[2 * k]),
                                         std::abs(reference[f][2 * k + 1] - expected[2 * k + 1])});
            }
    }
    void call(std::size_t r, std::size_t f) {
        check(r < 2 ? full[r].process(input[f], output[r]) : real[r - 2].process(input[f], output[r]),
              "Transform failed");
    }
    void verify_and_warm() {
        for (std::size_t r = 0; r < routes; ++r)
            if (active[r]) {
                for (std::size_t f = 0; f < frames; ++f) {
                    call(r, f);
                    for (std::size_t k = 0; k < n + 2; ++k) {
                        near(output[r][k], reference[f][k], tolerance);
                        errors[r] = std::max(errors[r], std::abs(double(output[r][k]) - reference[f][k]));
                    }
                }
                for (std::size_t w = 0; w < 64; ++w)
                    call(r, w % frames);
            }
    }
    void measure() {
        allocations = 0;
        watching = true;
        for (std::size_t b = 0; b < batches; ++b)
            for (std::size_t order = 0; order < routes; ++order) {
                const auto r = (b + order) % routes;
                if (!active[r])
                    continue;
                const auto start = std::chrono::steady_clock::now();
                for (std::size_t i = 0; i < batch; ++i)
                    call(r, (b * batch + i) % frames);
                elapsed[r][b] =
                    std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / batch;
            }
        watching = false;
        check(allocations == 0, "Process allocates");
    }
    void report() {
        constexpr std::array names{"CFFT2", "CFFT4", "RFFT2-scalar", "RFFT2-auto", "RFFT4-scalar", "RFFT4-auto"};
        for (std::size_t r = 0; r < routes; ++r) {
            std::cout << n << ',' << names[r] << ',';
            if (!active[r]) {
                std::cout << "unsupported,,,,,,,\n";
                continue;
            }
            auto &times = elapsed[r];
            std::sort(times.begin(), times.end());
            std::cout << (r < 2 ? "none" : real[r - 2].split_implementation()) << ',' << times[batches / 2] << ','
                      << times[batches * 95 / 100] << ','
                      << (r < 2 ? full[r].workspace_bytes() : real[r - 2].workspace_bytes()) << ',' << errors[r] << ','
                      << oracle_error << ',' << allocations << ',' << batch * batches << '\n';
        }
    }
};
void run(std::size_t n) {
    SizeBenchmark benchmark(n);
    benchmark.prepare();
    benchmark.verify_reference();
    benchmark.verify_and_warm();
    benchmark.measure();
    benchmark.report();
}
} // namespace
void *operator new(std::size_t n) {
    if (watching)
        ++allocations;
    if (auto *p = std::malloc(n ? n : 1))
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void *operator new(std::size_t n, std::align_val_t a) {
    if (watching)
        ++allocations;
    const auto alignment = std::size_t(a);
#if defined(_WIN32)
    auto *p = _aligned_malloc(n ? n : 1, alignment);
#else
    auto *p = std::aligned_alloc(alignment, ((n ? n : 1) + alignment - 1) / alignment * alignment);
#endif
    if (p)
        return p;
    throw std::bad_alloc();
}
void operator delete(void *p, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}
void operator delete(void *p, std::size_t, std::align_val_t a) noexcept { ::operator delete(p, a); }
int main() {
    try {
        std::cout << std::setprecision(9)
                  << "N,route,split,p50_us,p95_batch_mean_us,workspace_bytes,max_difference_vs_cfft2,max_probe_error_"
                     "cfft2_vs_dft,allocations,transforms\n";
        for (std::size_t n : {512, 1024, 2048, 4096, 8192, 16384})
            run(n);
    } catch (const std::exception &e) {
        watching = false;
        std::cerr << e.what() << '\n';
        return 1;
    }
}
