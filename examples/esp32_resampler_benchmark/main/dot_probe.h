#pragma once
#include <dsp_kernels/pcm_resampler.h>
#include <array>
#include <cmath>
#include <cstdio>
#include <esp_cpu.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace dot_probe {
using Function = float (*)(const float *, const float *, const float *, std::size_t, float);
#define PROBE_NOINLINE __attribute__((noinline))
inline void accumulate_four(const float *x, const float *weights, std::size_t k, float &v0, float &v1, float &v2,
                            float &v3) {
    v0 += x[k] * weights[k];
    v1 += x[k + 1] * weights[k + 1];
    v2 += x[k + 2] * weights[k + 2];
    v3 += x[k + 3] * weights[k + 3];
}
inline void accumulate_pair(const float *x, const float *a, const float *b, std::size_t k, float &a0, float &a1,
                            float &b0, float &b1) {
    const auto x0 = x[k], x1 = x[k + 1];
    a0 += x0 * a[k];
    b0 += x0 * b[k];
    a1 += x1 * a[k + 1];
    b1 += x1 * b[k + 1];
}
inline float interpolate_tail(const float *x, const float *a, const float *b, std::size_t k, std::size_t n,
                              float first, float second, float blend) {
    for (; k < n; ++k) {
        first += x[k] * a[k];
        second += x[k] * b[k];
    }
    return first + blend * (second - first);
}
// Frozen production arithmetic before the second optimization pass.
inline float old4_single(const float *x, const float *a, std::size_t n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    std::size_t k = 0;
    for (; k + 4 <= n; k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
    }
    float sum = (a0 + a1) + (a2 + a3);
    for (; k < n; ++k)
        sum += x[k] * a[k];
    return sum;
}
PROBE_NOINLINE inline float old4(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    std::size_t k = 0;
    if (blend == 0)
        return old4_single(x, a, n);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (; k + 4 <= n; k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
        accumulate_four(x, b, k, b0, b1, b2, b3);
    }
    float first = (a0 + a1) + (a2 + a3), second = (b0 + b1) + (b2 + b3);
    return interpolate_tail(x, a, b, k, n, first, second, blend);
}
inline float pair2_single(const float *x, const float *a, std::size_t n) {
    float a0 = 0, a1 = 0;
    std::size_t k = 0;
    for (; k + 2 <= n; k += 2) {
        a0 += x[k] * a[k];
        a1 += x[k + 1] * a[k + 1];
    }
    float sum = a0 + a1;
    for (; k < n; ++k)
        sum += x[k] * a[k];
    return sum;
}
PROBE_NOINLINE inline float pair2(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    float a0 = 0, a1 = 0, b0 = 0, b1 = 0;
    std::size_t k = 0;
    if (blend == 0)
        return pair2_single(x, a, n);
    for (; k + 2 <= n; k += 2) {
        accumulate_pair(x, a, b, k, a0, a1, b0, b1);
    }
    float first = a0 + a1, second = b0 + b1;
    return interpolate_tail(x, a, b, k, n, first, second, blend);
}
inline float blocks4_single(const float *x, const float *a, std::size_t n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    std::size_t k = 0;
    for (auto blocks = n / 4; blocks; --blocks, k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
    }
    float sum = (a0 + a1) + (a2 + a3);
    for (; k < n; ++k)
        sum += x[k] * a[k];
    return sum;
}
PROBE_NOINLINE inline float blocks4(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    std::size_t k = 0;
    if (blend == 0)
        return blocks4_single(x, a, n);
    float b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    for (auto blocks = n / 4; blocks; --blocks, k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
        accumulate_four(x, b, k, b0, b1, b2, b3);
    }
    float first = (a0 + a1) + (a2 + a3), second = (b0 + b1) + (b2 + b3);
    return interpolate_tail(x, a, b, k, n, first, second, blend);
}
PROBE_NOINLINE inline float fused4(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    if (blend == 0)
        return old4(x, a, b, n, blend);
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    std::size_t k = 0;
    for (; k + 4 <= n; k += 4) {
        s0 += x[k] * (a[k] + blend * (b[k] - a[k]));
        s1 += x[k + 1] * (a[k + 1] + blend * (b[k + 1] - a[k + 1]));
        s2 += x[k + 2] * (a[k + 2] + blend * (b[k + 2] - a[k + 2]));
        s3 += x[k + 3] * (a[k + 3] + blend * (b[k + 3] - a[k + 3]));
    }
    float sum = (s0 + s1) + (s2 + s3);
    for (; k < n; ++k)
        sum += x[k] * (a[k] + blend * (b[k] - a[k]));
    return sum;
}
PROBE_NOINLINE inline float scalar(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    float first = 0, second = 0;
    if (blend == 0) {
        for (std::size_t k = 0; k < n; ++k)
            first += x[k] * a[k];
        return first;
    }
    for (std::size_t k = 0; k < n; ++k) {
        first += x[k] * a[k];
        second += x[k] * b[k];
    }
    return first + blend * (second - first);
}
template <unsigned Unroll>
inline void accumulate_unrolled_single(const float *x, const float *a, std::size_t n, std::size_t &k,
                                       float &a0, float &a1, float &a2, float &a3) {
    for (; k + Unroll <= n; k += Unroll) {
#pragma GCC unroll 4
        for (unsigned j = 0; j < Unroll; j += 4) {
            const auto p = k + j;
            accumulate_four(x, a, p, a0, a1, a2, a3);
        }
    }
}
template <unsigned Unroll>
inline float unrolled4_single(const float *x, const float *a, std::size_t n) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0;
    std::size_t k = 0;
    accumulate_unrolled_single<Unroll>(x, a, n, k, a0, a1, a2, a3);
    for (; k + 4 <= n; k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
    }
    float sum = (a0 + a1) + (a2 + a3);
    for (; k < n; ++k)
        sum += x[k] * a[k];
    return sum;
}
template <unsigned Unroll>
inline void accumulate_unrolled_pair(const float *x, const float *a, const float *b, std::size_t n, std::size_t &k,
                                     float &a0, float &a1, float &a2, float &a3,
                                     float &b0, float &b1, float &b2, float &b3) {
    for (; k + Unroll <= n; k += Unroll) {
#pragma GCC unroll 4
        for (unsigned j = 0; j < Unroll; j += 4) {
            const auto p = k + j;
            accumulate_four(x, a, p, a0, a1, a2, a3);
            accumulate_four(x, b, p, b0, b1, b2, b3);
        }
    }
}
template <unsigned Unroll>
PROBE_NOINLINE float unrolled4(const float *x, const float *a, const float *b, std::size_t n, float blend) {
    float a0 = 0, a1 = 0, a2 = 0, a3 = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    std::size_t k = 0;
    if (blend == 0)
        return unrolled4_single<Unroll>(x, a, n);
    accumulate_unrolled_pair<Unroll>(x, a, b, n, k, a0, a1, a2, a3, b0, b1, b2, b3);
    for (; k + 4 <= n; k += 4) {
        accumulate_four(x, a, k, a0, a1, a2, a3);
        accumulate_four(x, b, k, b0, b1, b2, b3);
    }
    float first = (a0 + a1) + (a2 + a3), second = (b0 + b1) + (b2 + b3);
    return interpolate_tail(x, a, b, k, n, first, second, blend);
}
struct DotInputs {
    const float *x, *a, *b;
    std::size_t taps;
    float blend;
};
inline double dot_error(Function function, const DotInputs &input) {
    double first = 0, second = 0;
    for (std::size_t k = 0; k < input.taps; ++k) {
        first += double(input.x[k]) * input.a[k];
        second += double(input.x[k]) * input.b[k];
    }
    return std::abs(double(function(input.x, input.a, input.b, input.taps, input.blend)) -
                    (first + input.blend * (second - first)));
}
inline void measure_dot(Function function, const DotInputs &input, volatile float &sink,
                        std::array<std::uint32_t, 33> &times) {
    constexpr unsigned batch = 64;
    for (auto &t : times) {
        vTaskDelay(1);
        const auto begin = esp_cpu_get_cycle_count();
        for (unsigned i = 0; i < batch; ++i) {
            asm volatile("" ::: "memory");
            sink = function(input.x, input.a, input.b, input.taps, input.blend);
        }
        t = esp_cpu_get_cycle_count() - begin;
    }
}
inline bool run_dot_variant(const char *name, Function function, const DotInputs &input, volatile float &sink) {
    const auto error = dot_error(function, input);
    std::array<std::uint32_t, 33> times{};
    measure_dot(function, input, sink, times);
    std::sort(times.begin(), times.end());
    constexpr unsigned batch = 64;
    std::printf("{\"check\":\"dot_probe\",\"variant\":\"%s\",\"taps\":%zu,\"blend\":%.3f,\"max_abs\":%.9g,"
                "\"p50_cycles_per_dot\":%.3f,\"p95_cycles_per_dot\":%.3f}\n",
                name, input.taps, double(input.blend), error, double(times[16]) / batch, double(times[31]) / batch);
    std::fflush(stdout);
    return error < 2e-6;
}
inline bool compare_dot_variants(DotInputs input, volatile float &sink) {
    bool ok = true;
    for (const float blend : {0.f, .37f})
        for (const auto &entry : {std::pair{"old4", Function(old4)}, std::pair{"blocks4", Function(blocks4)},
                                  std::pair{"pair2", Function(pair2)}, std::pair{"unroll8", Function(unrolled4<8>)},
                                  std::pair{"unroll16", Function(unrolled4<16>)}, std::pair{"fused4", Function(fused4)},
                                  std::pair{"scalar", Function(scalar)}}) {
            input.blend = blend;
            ok = run_dot_variant(entry.first, entry.second, input, sink) && ok;
        }
    return ok;
}
inline void fill_dot_input(std::vector<float> &input) {
    for (std::size_t k = 0; k < input.size(); ++k)
        input[k] = .3f * std::sin(.1f * k) + .2f * std::cos(.07f * k);
}
inline bool run_dot_quality(dsp_kernels::PcmResampler::Quality quality, volatile float &sink, bool &ok) {
    dsp_kernels::PcmResampler filter;
    if (!filter.prepare(48000, 32768, 1, quality))
        return false;
    const auto g = filter.filter_geometry();
    const auto w = filter.coefficients();
    std::vector<float> x(g.taps);
    fill_dot_input(x);
    const float *a = w.data() + 8 * g.taps;
    const float *b = a + g.taps;
    ok = compare_dot_variants({x.data(), a, b, g.taps, 0}, sink) && ok;
    return true;
}
inline bool run() {
    volatile float sink = 0;
    bool ok = true;
    for (const auto quality : {dsp_kernels::PcmResampler::Quality::Full, dsp_kernels::PcmResampler::Quality::Compact}) {
        if (!run_dot_quality(quality, sink, ok))
            return false;
    }
    (void)sink;
    return ok;
}
#undef PROBE_NOINLINE
} // namespace dot_probe
