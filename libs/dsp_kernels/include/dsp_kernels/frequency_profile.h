#pragma once
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/prepared_resampler.h>
#include <cstdint>
#include <string>
#include <optional>

namespace dsp_kernels {
enum class ChannelPolicy { Mean, Select };
struct ProfileCompatibility {
    double sample_rate = 32768;
    std::size_t fft_size = 4096;
    WindowFunction window = WindowFunction::Hann;
    // Zero selects sums derived from the explicitly declared window function.
    double coherent_gain = 0, power_gain = 0;
    bool include_dc = true, include_nyquist = true;
    ChannelPolicy channel_policy = ChannelPolicy::Mean;
    std::uint32_t channel_index = 0;
    SpectrumProjectionOptions projection;
    double reference = 1;
    double floor_db = -120;
    // Policy used to convert native PCM to sample_rate. Native identity is the
    // identity branch of the canonical FIR policy, including a missing node.
    ResamplerMode resampler_mode = ResamplerMode::ExistingFir;
    unsigned resampler_quality = 5;
    std::uint32_t resampler_design_version = PreparedResampler::design_version;
};
enum class ProfileGainMode { MeanEqualization, PeakNormalization };
struct ProfileTraining {
    // Legacy profiles without a mode remain mean-based.
    ProfileGainMode gain_mode = ProfileGainMode::MeanEqualization;
    double silence_rms = 0.0001;
    double min_gain = 0.1, max_gain = 100;
    std::size_t smoothing_radius = 8;
    std::size_t hop = 273;
    // Nonzero numerator selects rational live-equivalent cadence; zero uses explicit hop.
    std::uint64_t interval_numerator = 1, interval_denominator = 120;
};
struct CompositionStatistics {
    std::string source;
    std::uint64_t frames = 0, active_frames = 0, samples = 0;
    double rms = 0;
    // Raw nonzero windows are retained independently of the diagnostic threshold.
    std::uint64_t digital_silence_frames = 0, quiet_frames = 0;
    double quiet_rms = 0, minimum_frame_rms = 0, maximum_frame_rms = 0;
    double measured_max_hz = 0;
    std::vector<double> quiet_mean_spectrum, quiet_peak_spectrum;
    std::vector<double> mean_spectrum;
    std::vector<double> peak_spectrum;
    std::vector<double> frequency_presence;
    // Zero rates identify legacy measurements on the profile's common grid.
    double source_sample_rate = 0, sample_rate = 0;
    struct RmsBand {
        unsigned index = 0;
        std::uint64_t frames = 0;
        double square_sum = 0;
        std::vector<float> mean_spectrum, peak_spectrum;
    };
    unsigned rms_statistics_version = 0;
    std::vector<RmsBand> rms_bands;
};
enum class PeakAggregation { MeanTrackMaxima, GlobalMaximum, UpperPercentile };
enum class CurveApproximation { Polynomial, PiecewiseSmooth };
struct FrequencyCurveOptions {
    PeakAggregation aggregation = PeakAggregation::MeanTrackMaxima;
    CurveApproximation approximation = CurveApproximation::PiecewiseSmooth;
    double percentile = 90, regularization_db = -120, max_gain = 100;
    unsigned polynomial_degree = 6;
    std::size_t segments = 64;
};
struct FrequencyCurve {
    FrequencyCurveOptions options;
    double minimum_hz = 0, maximum_hz = 0, fit_rmse_db = 0;
    // Polynomial coefficients use Chebyshev basis on [-1,1] in linear Hz.
    // Piecewise knots hold log amplitude, with monotone cubic Hermite slopes.
    std::vector<double> coefficients, knot_hz, log_amplitude, slopes;
    std::vector<double> measured_spectrum;
};
struct FrequencyProfile {
    ProfileCompatibility compatibility;
    ProfileTraining training;
    std::vector<float> gains;
    std::vector<double> mean_spectrum;
    std::vector<double> peak_spectrum;
    std::vector<double> frequency_presence;
    std::vector<CompositionStatistics> compositions;
    std::optional<FrequencyCurve> curve;
};
// Cold operations only; processing continues to use the prepared ArrayMultiply.
bool build_frequency_curve(const ProfileCompatibility&,std::span<const CompositionStatistics>,
    const FrequencyCurveOptions&,FrequencyProfile&,std::string& error);
bool valid_frequency_curve(const FrequencyProfile&);
double frequency_curve_gain(const FrequencyProfile&,double hz) noexcept;
double frequency_curve_amplitude(const FrequencyProfile&,double hz) noexcept;
bool resample_frequency_profile(const FrequencyProfile&,const ProfileCompatibility&,
    FrequencyProfile&,std::string& error);
enum class ProfileMatchScope { Exact, SignalOnly };
enum class ProfileMismatchReason {
    InvalidSettings, InvalidCadence, SampleRate, FftSize, Window,
    CoherentGain, PowerGain, Projection, DcLayout, NyquistLayout,
    ChannelPolicy, ChannelIndex, ResamplerMode, ResamplerQuality,
    ResamplerDesignVersion, Reference, Floor, FrequencyGridLength,
    FrequencyPresence, InvalidCoefficient
};
// Numeric coefficient validation; an optional offline function is checked separately
// by valid_frequency_curve before persistence or resampling.
// Callers translate reasons instead of reimplementing matching.
std::vector<ProfileMismatchReason> profile_mismatches(const FrequencyProfile&,
    const ProfileCompatibility&,ProfileMatchScope scope=ProfileMatchScope::Exact);
WindowScaling profile_window_scaling(const ProfileCompatibility&) noexcept;
bool valid_profile_settings(const ProfileCompatibility&, const ProfileTraining&, std::string& error);
bool validate_profile(const FrequencyProfile&, const ProfileCompatibility& expected, std::string& error);
// Independent per-bin normalization of observed spectral maxima to 1.0.
// Coefficients are boost-only (>=1) and bounded by max_gain. Confidence in
// measured energy fades continuously to unity below the regularization floor.
// Never change output on an invalid input.
bool build_frequency_peak_compensation_curve(std::span<const double> peak_spectrum,
    double floor, double max_gain, std::vector<float>& output);
// Equal composition weighting; a composition with no active windows contributes nothing.
bool build_frequency_profile(const ProfileCompatibility&, const ProfileTraining&,
    std::span<const CompositionStatistics>, FrequencyProfile& output, std::string& error);
// Exact checks all metadata; SignalOnly checks the live signal/frontend,
// while reference/floor are validated as profile-owned training settings.
class PreparedFrequencyNormalizer {
  public:
    bool prepare(std::span<const float> gains);
    bool prepare_manual(std::size_t bins, float gain);
    // Supports exact in-place use; failed calls leave output unchanged.
    bool process(std::span<const float> input, std::span<float> output) const noexcept;
    std::size_t workspace_bytes() const noexcept { return gains_.capacity() * sizeof(float); }
  private:
    std::vector<float> gains_;
};
}
