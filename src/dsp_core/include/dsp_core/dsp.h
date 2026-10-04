#pragma once
#include <cstddef>
#include <span>
#include <optional>
#include <vector>

namespace dsp_core {
#ifndef PORTABLE_DSP_MAX_FFT_SIZE
#define PORTABLE_DSP_MAX_FFT_SIZE 4096
#endif
inline constexpr std::size_t max_fft_samples = PORTABLE_DSP_MAX_FFT_SIZE;
static_assert(max_fft_samples >= 512 && max_fft_samples <= 4096 &&
              (max_fft_samples & (max_fft_samples - 1)) == 0);
inline constexpr std::size_t max_radix4_samples =
    (max_fft_samples & 0x55555555) ? max_fft_samples : max_fft_samples / 2;
// Payload bytes, excluding allocator overhead, object headers, stack, frame
// copies, AudioWindow storage and executable/constant tables. Caller buffers
// describe one stereo processing set; history is one bands-wide float history.
// Estimates use required element counts. Reprepared vectors can retain larger
// capacities; workspace_bytes() on each live processor reports actual capacity.
// Shared ESP tables are process-wide heap allocations, separate from instances.
// Shared table estimates assume this library initializes the backend first;
// an external owner can supply a different table budget.
struct MemoryRequirements {
    std::size_t fft_scratch_bytes = 0, mel_boundaries_bytes = 0;
    std::size_t window_bytes = 0, weights_bytes = 0;
    std::size_t stereo_input_bytes = 0, stereo_spectrum_bytes = 0, stereo_mel_bytes = 0;
    std::size_t queue_payload_bytes = 0, history_payload_bytes = 0;
    std::size_t shared_radix2_tables_bytes = 0, shared_radix4_tables_bytes = 0;
    std::size_t workspace_bytes() const {
        return fft_scratch_bytes + mel_boundaries_bytes;
    }
    std::size_t instance_payload_bytes() const {
        return workspace_bytes() + window_bytes + weights_bytes + stereo_input_bytes + stereo_spectrum_bytes +
               stereo_mel_bytes + queue_payload_bytes + history_payload_bytes;
    }
};
// Rejects unsupported application budgets: power-of-two N in [512,max_fft_samples],
// bands <= 50, PCM queue slots <= 16, history rows <= 2000.
std::optional<MemoryRequirements> estimate_memory(std::size_t samples, std::size_t bands, std::size_t queue_slots = 0,
                                                  std::size_t history_rows = 0);
// Instances own their scratch space. prepare may allocate; process never does.
// One instance belongs to one processing thread; different instances are independent.
// Shared vendor tables are never resized or deinitialized by this library.
// prepare rejects undersized tables left by an earlier direct vendor init.
// Direct vendor init/deinit must not run concurrently with library operations,
// and vendor deinit must not run while any prepared plan remains in use.
class FFT {
  public:
    bool prepare(std::size_t samples, bool radix4 = false);
    bool process(std::span<const float> input, std::span<float> output, bool hartley = false);
    std::size_t workspace_bytes() const {
        return scratch_.capacity() * sizeof(float);
    }

  private:
    std::vector<float> scratch_;
    bool radix4_ = false;
};
class MelFilterBank {
  public:
    bool prepare(std::size_t bins, std::size_t sample_rate, std::size_t bands);
    bool process(std::span<const float> input, std::span<float> output) const;
    std::size_t workspace_bytes() const {
        return boundaries_.capacity() * sizeof(std::size_t);
    }

  private:
    std::size_t bins_ = 0, bands_ = 0;
    std::vector<std::size_t> boundaries_;
};
// Applies the supplied window and Bark weights in the same order as Analyzer.
// Input spans are replaced with windowed samples for legacy callbacks.
class StereoProcessor {
  public:
    bool prepare(std::size_t samples, std::size_t rate, std::size_t bands = 50);
    bool process(std::span<float> left, std::span<float> right, std::span<const float> window,
                 std::span<const float> weights, std::span<float> spectrum_left, std::span<float> spectrum_right,
                 std::span<float> mel_left = {}, std::span<float> mel_right = {});
    bool process_mel(std::span<const float> left, std::span<const float> right, std::span<float> mel_left,
                     std::span<float> mel_right) const;
    std::size_t workspace_bytes() const {
        return fft_.workspace_bytes() + mel_.workspace_bytes();
    }

  private:
    FFT fft_;
    MelFilterBank mel_;
};
} // namespace dsp_core
