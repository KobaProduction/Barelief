#include "dsp_core/dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <new>
#include <numbers>

static bool watching = false;
static unsigned allocations = 0;
#ifdef DSP_KERNELS_WRAP_ALLOCATOR
// GNU linker wrappers also observe the vendored C backend's heap calls.
extern "C" void *__real_malloc(std::size_t);
extern "C" void *__real_calloc(std::size_t, std::size_t);
extern "C" void *__real_realloc(void *, std::size_t);
extern "C" void *__wrap_malloc(std::size_t n) {
    if (watching) ++allocations;
    return __real_malloc(n);
}
extern "C" void *__wrap_calloc(std::size_t n, std::size_t size) {
    if (watching) ++allocations;
    return __real_calloc(n, size);
}
extern "C" void *__wrap_realloc(void *p, std::size_t n) {
    if (watching) ++allocations;
    return __real_realloc(p, n);
}
#endif
void *operator new(std::size_t n) {
    if (watching) ++allocations;
    if (auto p = std::malloc(n ? n : 1)) return p;
    std::abort();
}
void *operator new[](std::size_t n) { return ::operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
static void check(bool ok, const char *message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}
int main() {
#ifdef DSP_KERNELS_WRAP_ALLOCATOR
    // Verify the C linker guard itself before relying on a zero count.
    void *(*volatile probe)(std::size_t) = std::malloc;
    watching = true;
    auto *probe_memory = probe(8);
    watching = false;
    std::free(probe_memory);
    check(allocations > 0, "C allocation guard is inactive");
    allocations = 0;
#endif
    // Eight changing silent tones, both FFT radices, every supported profile.
    for (std::size_t n = 512; n <= dsp_core::max_fft_samples; n *= 2) {
        dsp_core::StereoProcessor stereo;
        dsp_core::FFT reference;
        dsp_core::MelFilterBank mel;
        check(stereo.prepare(n, 48000, 50) && reference.prepare(n, (n & 0x55555555) != 0) &&
                  mel.prepare(n / 2, 48000, 50), "prepare failed");
        std::vector<float> left(n), right(n), expected_left(n), expected_right(n), win(n), weights(n / 2);
        std::vector<float> sl(n / 2), sr(n / 2), expected_sl(n / 2), expected_sr(n / 2);
        std::vector<float> ml(50), mr(50), expected_ml(50), expected_mr(50);
        dsp_core::hann_window(win);
        dsp_core::bark_weights(weights, 48000.f / n);
        const auto workspace = stereo.workspace_bytes();
        for (unsigned tick = 0; tick < 5; ++tick) {
            for (std::size_t i = 0; i < n; ++i) {
                float x = 0, y = 0;
                for (unsigned tone = 0; tone < 8; ++tone) {
                    const double phase = 2 * std::numbers::pi * (7 + tone * 11 + tick * .25) * i / n;
                    x += float(std::sin(phase)) * (.02f * (tone + 1));
                    y += float(std::cos(phase)) * (.01f * (8 - tone));
                }
                left[i] = x; right[i] = tick == 1 ? -x : tick == 2 ? 0 : y;
                expected_left[i] = left[i] * win[i]; expected_right[i] = right[i] * win[i];
            }
            check(reference.process(expected_left, expected_sl) && reference.process(expected_right, expected_sr),
                  "reference FFT failed");
            for (std::size_t i = 0; i < sl.size(); ++i) {
                expected_sl[i] *= weights[i]; expected_sr[i] *= weights[i];
            }
            check(mel.process(expected_sl, expected_ml) && mel.process(expected_sr, expected_mr), "reference Mel failed");
            watching = true;
            const bool ok = stereo.process(left, right, win, weights, sl, sr, ml, mr);
            watching = false;
            check(ok && allocations == 0, "full prepared stereo kernel allocated or failed");
            check(left == expected_left && right == expected_right && sl == expected_sl && sr == expected_sr &&
                      ml == expected_ml && mr == expected_mr, "stereo numerical order or channel behavior changed");
            check(stereo.workspace_bytes() == workspace, "workspace changed during processing");
            // FFT magnitude and Hartley routes and Mel-only route remain allocation-free.
            watching = true;
            const bool extra = reference.process(left, sl, true) && stereo.process_mel(sl, sr, ml, mr);
            watching = false;
            check(extra && allocations == 0, "Hartley or Mel-only kernel allocated");
        }
        std::fill(weights.begin(), weights.end(), std::numeric_limits<float>::infinity());
        check(stereo.process(left, right, win, weights, sl, sr, ml, mr), "nonfinite weight process failed");
        check(std::all_of(sl.begin(), sl.end(), [](float x) { return x == 0; }) &&
                  std::all_of(sr.begin(), sr.end(), [](float x) { return x == 0; }), "nonfinite spectra sanitization changed");
    }
    std::cout << "Full stereo/FFT/Hartley/Mel characterization and allocation guard passed\n";
}
