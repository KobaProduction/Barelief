#pragma once
#include <dsp_kernels/frame_sampling.h>
#include <dsp_kernels/tempogram.h>
#include <dsp_kernels/mel_filter_bank.h>
#include <dsp_kernels/log_frequency_filter_bank.h>
#include <dsp_kernels/log_compression.h>
#include <dsp_kernels/complex_values.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace dsp_kernels {
enum class SpectralFilterMode {None,Logarithmic,Explicit,Mel};
struct SpectralFrameRecipeConfig {
    FrameSamplingConfig framing;
    float sample_rate=44100;
    double frame_rate=0; // Explicit clock setting: >0 overrides framing.hop_samples.
    std::size_t fft_size=0,window_samples=0; // Short windows are centered in the frame.
    WindowFunction window=WindowFunction::Hann;
    bool periodic=false,circular_shift=false,include_nyquist=false;
    SpectralFilterMode filtering=SpectralFilterMode::None;
    TemporalPadding padding=TemporalPadding::Constant;
    // Exponent of the raw FFT magnitude, before filter-bank projection.
    // |X|^2 here is not calibrated Power, PSD, or BinPower; source recipes
    // deliberately do not divide by C/E or apply one-sided doubling.
    float power=1;
    bool physical_filter_axis=false; // Source bank grid can instead use exact Fs*k/N.
    // Filtered-spectrogram processor defaults (12 bands, 30Hz lower corner).
    LogFrequencyFilterConfig logarithmic_filters{.bands_per_octave=12};
    MelFilterBankConfig mel_filters{.fmin_hz=30,.fmax_hz=17000,.norm_p=1,.bands=12,
        .normalization=MelNormalization::Lp,.construction=MelConstruction::NearestBinTriangles};
    bool logarithm=false;
    float logarithm_multiply=1,logarithm_add=1;
    LogarithmBase logarithm_base=LogarithmBase::Ten;
    ComplexMagnitudeNorm magnitude_norm=ComplexMagnitudeNorm::Hypot;
    ZeroMagnitudePhase zero_phase=ZeroMagnitudePhase::Atan2;
    // DesktopReference selects source Complex64 ScaledRatioFma when the norm
    // selector is Hypot. PortableFloat32 and explicit SquaredNorm keep theirs.
    NumericalPrecision precision=NumericalPrecision::PortableFloat32;
};
struct ExplicitSpectralFilters {
    std::size_t bands=0;
    std::span<const float> weights; // [band][FFT bin], copied at preparation
    std::span<const double> centers_hz;
};
struct SpectralFrameOutputs {
    std::span<float> features,phase,magnitude,powered,linear;
};
struct SpectralFrameValues {
    // magnitude is raw |X| under the explicit source norm; powered is |X|^p.
    // Neither view acquires window calibration from metadata or Quantity.
    std::span<const float> complex,magnitude,phase,powered,linear,features;
};
// Source spectral frontend: frame schedule -> symmetric/explicit window ->
// raw RFFT/DFT with independent FFT length/truncation/zero-phase embedding ->
// optional explicit linear bank -> optional logarithm. Mono normalized Float32
// PCM; no resampling, UInt8, dB or hidden normalization.
// One frame is processed at a time, in any order. PCM is borrowed synchronously;
// no input/full-spectrogram history is copied or retained. Phase always refers
// to the original FFT bins, independent of feature filtering/logarithm.
// frequencies_hz is the physical FFT axis. source_filter_frequencies_hz keeps
// the original fft_frequencies(RETURNED_BINS,Fs) convention for reproducing
// filter coefficients (different with Nyquist/odd FFT); it is not a plot axis.
class PreparedSpectralFrameRecipe {
public:
    // No allocation/FFT preparation. Conservative owned dynamic cold-peak
    // bound, including temporary filter/window construction buffers; excludes
    // instance headers, allocator overhead, backend shared tables and stack.
    static bool storage_bytes(std::size_t maximum_samples,SpectralFrameRecipeConfig,
        std::span<const float> window,ExplicitSpectralFilters,std::size_t&) noexcept;
    static bool geometry(std::size_t maximum_samples,SpectralFrameRecipeConfig,
        ExplicitSpectralFilters,std::size_t& frames,std::size_t& features) noexcept;
    bool prepare(std::size_t maximum_samples,SpectralFrameRecipeConfig={},
                 std::span<const float> window={},ExplicitSpectralFilters={});
    std::size_t frames(std::size_t samples) const noexcept {return sampling_.frames(samples);}
    std::size_t features() const noexcept {return features_;}
    std::size_t phase_bins() const noexcept {return frequencies_.size();}
    std::span<const float> frequencies_hz() const noexcept {return frequencies_;}
    std::span<const float> source_filter_frequencies_hz() const noexcept {return source_filter_frequencies_;}
    std::span<const double> source_filter_centers_hz() const noexcept {return filtering_.centers_hz();}
    std::span<const FilterBandSupport> supports() const noexcept {return supports_;}
    std::span<const float> filter_weights() const noexcept {return filtering_.weights();}
    std::span<const float> window_coefficients() const noexcept {return spectrum_.window_coefficients();}
    double hop_samples() const noexcept {return config_.framing.hop_samples;}
    bool process_frame(std::span<const float> pcm,std::size_t frame,
                       std::span<float> features,std::span<float> phase={}) noexcept;
    bool process_frame(std::span<const float> pcm,std::size_t frame,SpectralFrameOutputs) noexcept;
    // Explicit independently usable stages. transform_frame publishes only raw
    // complex bins. project_frame consumes explicit raw bins and computes only
    // requested value stages, never re-running FFT or retaining caller memory.
    bool transform_frame(std::span<const float> pcm,std::size_t frame) noexcept;
    bool project_frame(std::span<const float> complex,SpectralFrameOutputs) noexcept;
    // Owned borrowed intermediate views, invalidated by the next processing
    // call (including failure), or successful prepare. Phase/magnitude use FFT
    // bins; linear/features use filter bands. No full input history is cloned.
    SpectralFrameValues values() const noexcept;
    void invalidate_values() noexcept {stages_=0;}
    std::size_t workspace_bytes() const noexcept;
private:
    static bool resolve_config(SpectralFrameRecipeConfig&) noexcept;
    bool configure(std::size_t,SpectralFrameRecipeConfig,std::span<const float>,ExplicitSpectralFilters);
    bool prepare_spectrum(std::span<const float>);
    TemporalWindowConfig spectrum_window() const noexcept;
    bool prepare_spectrum_axes(std::size_t);
    bool prepare_window_coefficients(std::span<const float>,std::vector<float>&) const;
    bool prepare_reference_window_coefficients(std::span<const float>,std::vector<double>&) const;
    bool prepare_filter(ExplicitSpectralFilters);
    bool explicit_filter(std::size_t,std::span<const float>,std::span<const double>);
    bool valid_outputs(SpectralFrameOutputs) const noexcept;
    bool calculate_values(SpectralFrameOutputs,std::uint8_t&) noexcept;
    bool calculate_derived(SpectralFrameOutputs,std::uint8_t&) noexcept;
    bool filter_values() noexcept;
    bool feature_values() noexcept;
    void publish(SpectralFrameOutputs) const noexcept;
    bool prepare_mel_filter();
    SpectralFrameRecipeConfig config_;
    PreparedFrameSampling sampling_;
    PreparedFourierTempogram spectrum_;
    MelFilterBank filtering_;
    std::size_t maximum_samples_=0,features_=0;
    std::vector<float> complex_,magnitude_,phase_,powered_,linear_,filtered_,frequencies_,source_filter_frequencies_;
    std::vector<FilterBandSupport> supports_;
    std::uint8_t stages_=0;
};
}
