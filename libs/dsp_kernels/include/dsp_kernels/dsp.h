#pragma once
#include <dsp_kernels/window_math.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>
#include <string_view>
#include <vector>
#include <signal_contracts/data_frame.h>
#include <dsp_kernels/fft_storage.h>

namespace dsp_kernels {
enum class Backend { Ansi, EspDsp };
Backend compiled_backend() noexcept;
inline constexpr std::size_t max_mel_bands = 50;
void hann_window(std::span<float> output);

void window(std::span<float> output, WindowFunction function);
// Immutable bank shared across graph revisions. All four windows for the
// declared geometry are prepared together. Variable extents use prepared
// rotations rather than trigonometry or a quadratic table of every length.
class PreparedWindows {
  public:
    PreparedWindows() = default;
    explicit PreparedWindows(std::size_t maximum);
    // Cold preparation only. Invalid geometry is refused before allocation and
    // preserves an existing bank. Allocations follow the other DSP prepare APIs.
    bool prepare(std::size_t maximum);
    bool prepared() const noexcept { return prepared_; }
    std::size_t maximum() const noexcept { return maximum_; }
    std::span<const float> coefficients(WindowFunction function) const noexcept;
    bool coefficients(WindowFunction function, std::span<float> output) const noexcept;
    static bool storage_bytes(std::size_t maximum, std::size_t& bytes) noexcept;
  private:
    std::size_t maximum_ = 0;
    bool prepared_ = false;
    std::vector<float> bank_;
    struct Rotation { double cosine, sine; };
    std::vector<Rotation> rotations_;
};
void bark_weights(std::span<float> output, float frequency_step);
#ifndef PORTABLE_DSP_MAX_FFT_SIZE
#define PORTABLE_DSP_MAX_FFT_SIZE 4096
#endif
inline constexpr std::size_t max_fft_samples = PORTABLE_DSP_MAX_FFT_SIZE;
static_assert(max_fft_samples >= 512 && max_fft_samples <= 16384 &&
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
namespace detail { struct FftBenchmarkAccess; }
class FFT {
  public:
    // Complex input -> full, unnormalized forward DFT. Both input planes are
    // required and have N elements; output contains N interleaved Re/Im pairs.
    // No symmetry is assumed: imaginary DC/Nyquist and negative frequencies
    // are preserved. No sample rate is needed. Failure leaves output unchanged.
    bool process_complex(std::span<const float> real, std::span<const float> imaginary,
                         std::span<float> complex_output) noexcept;
    // Real, already-windowed PCM -> unnormalized forward complex DFT.
    // Same N/2+1 interleaved Re/Im contract as PreparedRfft, using full CFFT(N).
    bool prepare(std::size_t samples, bool radix4 = false);
    bool process(std::span<const float> input, std::span<float> complex_output) noexcept;
    void reset() noexcept;
    std::size_t samples() const noexcept { return scratch_.size() / 2; }
    std::size_t bins() const noexcept { return samples() ? samples() / 2 + 1 : 0; }
    std::size_t workspace_bytes() const noexcept { return scratch_.capacity() * sizeof(float); }
  private:
    friend struct detail::FftBenchmarkAccess;
    bool publish_transform(std::span<float> output, int scale, bool real_input) noexcept;
    bool restore_transform(std::size_t output_size, int scale, bool real_input) noexcept;
    detail::FftStorage scratch_;
    bool radix4_ = false;
};
// Explicit composite spectrum operation for magnitude-valued graph consumers.
// This is not the raw FFT interface. It publishes N/2 abs(X)/N values;
// This is not calibrated peak amplitude.
class MagnitudeSpectrum {
  public:
    bool prepare(std::size_t samples, bool radix4 = false);
    bool process(std::span<const float> input, std::span<float> output);
    // Reads and writes one plane directly; the instance scratch is the only
    // intermediate payload. Views remain borrowed for this synchronous call.
    signal_contracts::Status process_plane(const signal_contracts::NumericView& input,
        signal_contracts::MutableNumericView& output, std::size_t plane);
    std::size_t workspace_bytes() const {
        return scratch_.capacity() * sizeof(float);
    }

  private:
    detail::FftStorage scratch_;
    bool radix4_ = false;
};
// Statistics of the actual coefficients applied before the RFFT. Windows are
// symmetric; zero padding (if any) must be included in samples explicitly.
struct WindowScaling {
    std::size_t samples = 0;
    double sum = 0, sum_squares = 0;
    double coherent_gain() const noexcept { return samples ? sum / double(samples) : 0; }
};
std::optional<WindowScaling> window_scaling(std::span<const float> coefficients) noexcept;
// Unnormalized forward real DFT, exp(-i*2*pi*k*n/N). Output is N/2+1
// interleaved {Re,Im} pairs, including exactly real DC and Nyquist. Input has
// already been windowed. Radix-4 uses two radix-4 subtransforms plus one radix-2
// butterfly when the packed complex length is not a power of four.
// prepare may allocate; process never allocates. Failed preparation disables
// the plan. Failed processing leaves caller output unchanged. One owner/thread.
class PreparedRfft {
  public:
    enum class Split { Automatic, Scalar };
    struct Options { bool platform_optimizations = true; };
    // Null options select maximum available optimizations. Copied at prepare.
    // Runtime selection does not unload code; use the firmware build profile
    // to exclude platform kernels from IRAM.
    bool prepare(std::size_t samples, bool radix4 = false, Split split = Split::Automatic,
                 const Options* options = nullptr);
    bool process(std::span<const float> input, std::span<float> complex_output) noexcept;
    void reset() noexcept;
    const char* split_implementation() const noexcept;
    std::size_t samples() const noexcept { return samples_; }
    std::size_t bins() const noexcept { return samples_ ? samples_ / 2 + 1 : 0; }
    std::size_t workspace_bytes() const noexcept {
        return (scratch_.capacity() + twiddles_.capacity() + mixed_.capacity()) * sizeof(float);
    }
  private:
    friend struct detail::FftBenchmarkAccess;
    bool transform_prepared() noexcept;
    bool transform_input(std::span<const float> input,bool fused,int& scale) noexcept;
    bool prepare_twiddles(std::size_t samples, std::size_t packed, bool mixed, Split split) noexcept;
    int input_scale(std::span<const float> input, bool fused) noexcept;
    void pack_mixed(std::span<const float> input, float factor, bool fused) noexcept;
    void pack_half(std::span<const float> input, float factor, int scale) noexcept;
    bool restore_output(int scale) noexcept;
    bool transform_mixed(std::size_t packed, bool fused) noexcept;
    void mix_quarters(std::size_t packed) noexcept;
    void split_spectrum(std::size_t packed) noexcept;
    std::size_t samples_ = 0;
    bool radix4_ = false, mixed_radix_ = false, platform_optimizations_ = true;
    Split split_ = Split::Automatic;
    detail::FftStorage scratch_, twiddles_, mixed_;
};
enum class SpectrumProjection {
    Real, Imaginary, Phase, Magnitude, PeakAmplitude, Power, Psd
};
struct SpectrumProjectionOptions {
    SpectrumProjection representation = SpectrumProjection::Power;
    bool compensate = true;
    bool operator==(const SpectrumProjectionOptions&) const = default;
};
std::span<const SpectrumProjection> amplitude_spectrum_projections() noexcept;
std::string_view spectrum_projection_name(SpectrumProjection) noexcept;
bool parse_spectrum_projection(std::string_view, SpectrumProjection&) noexcept;
bool is_amplitude_spectrum_projection(SpectrumProjection) noexcept;
// The common prepared math for planar graph inputs and interleaved batch FFT.
// paired marks a bin with a conjugate partner in a one-sided spectrum.
class PreparedSpectrumScaling {
  public:
    bool prepare(double sample_rate, double window_sum, double window_square_sum,
                 SpectrumProjectionOptions options) noexcept;
    bool project(float real, float imaginary, bool paired, float& output) const noexcept;
  private:
    SpectrumProjectionOptions options_{};
    double amplitude_scale_=1, power_scale_=1, psd_scale_=1;
    bool ready_=false;
};

// Explicit numerical interpretation, independent of a frame's descriptive quantity.
struct SpectrumPowerPolicy {
    SpectrumProjection representation=SpectrumProjection::Power;
    std::uint32_t first_bin=0;
    bool include_nyquist=true;
    double bin_hz=1;
    // C=sum(w): calibrates raw Magnitude=|DFT| to mean-square power.
    double coherent_gain=1;
    // Equivalent noise bandwidth Fs*sum(w^2)/sum(w)^2, supplied by this
    // operation's setting. It is generally wider than bin_hz for tapered windows.
    double power_bandwidth_hz=1;
};
class PreparedSpectrumPower {
public:
    bool prepare(std::size_t input_bins,SpectrumPowerPolicy policy) noexcept;
    std::size_t output_bins() const noexcept {return ready_?input_bins_+policy_.first_bin:0;}
    // Allocation-free. Failure leaves output unchanged; exact in-place is
    // supported when first_bin=0, other overlapping layouts are refused.
    bool process(std::span<const float> input,std::span<float> output) const noexcept;
private:
    bool convert(float value,std::size_t index,float& output) const noexcept;
    bool valid_output(std::span<const float> input,std::span<float> output) const noexcept;
    bool valid_values(std::span<const float> input) const noexcept;
    SpectrumPowerPolicy policy_{};
    std::size_t input_bins_=0;
    bool ready_=false;
};
// Separate prepared projection preserves the raw DFT. For d=1 at endpoints,
// d=2 elsewhere: peak=d*abs(X)/sum(w), power=d*abs(X)^2/sum(w)^2,
// PSD=d*abs(X)^2/(Fs*sum(w^2)). Power is a tonal mean-square estimator,
// PSD has sample^2/Hz units. Phase is radians, atan2(Im,Re), with phase(0)=0.
// Compensation is explicit in options. Logarithmic display belongs to the renderer.
// Output must not overlap raw input. Failure leaves output unchanged.
class PreparedSpectrumProjection {
  public:
    bool prepare(std::size_t samples, double sample_rate, WindowScaling scaling,
                 SpectrumProjectionOptions options);
    bool process(std::span<const float> complex_input, std::span<float> output) noexcept;
    std::size_t workspace_bytes() const noexcept { return scratch_.capacity() * sizeof(float); }
  private:
    std::size_t samples_ = 0;
    PreparedSpectrumScaling scaling_;
    std::vector<float> scratch_;
};
class MelFilterBank {
  public:
    // Returns one coefficient per Mel band: sum(input magnitude * triangle).
    // This is linear magnitude, not squared power, dB or area-normalized energy.
    // Equal Mel spacing selects triangle centres. Optional explicit frequencies
    // are prepared once; otherwise bins use k*sample_rate/(2*bins).
    // Bins <= max_fft_samples/2 and bands <= max_mel_bands. Zero bins/rate
    // retain legacy zero-energy behavior. Invalid preparation disables the plan.
    bool prepare(std::size_t bins, std::size_t sample_rate, std::size_t bands,
                 std::span<const double> frequencies_hz = {});
    bool process(std::span<const float> input, std::span<float> output) const;
    double center_hz(std::size_t band) const noexcept { return band<bands_?boundaries_[band+1]:0; }
    std::size_t workspace_bytes() const {
        return (boundaries_.capacity() + weights_.capacity()) * sizeof(double);
    }

  private:
    std::size_t bins_ = 0, bands_ = 0;
    std::vector<double> boundaries_; // Shared continuous-Hertz triangles.
    std::vector<double> weights_; // [band][bin], prepared for the declared grid.
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
    MagnitudeSpectrum fft_;
    MelFilterBank mel_;
};
} // namespace dsp_kernels
