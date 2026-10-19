#pragma once
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/prepared_resampler.h>
#include <cstdint>
#include <string>

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
struct ProfileTraining {
    double silence_rms = 0.0001;
    double min_gain = 0.1, max_gain = 10;
    std::size_t smoothing_radius = 8;
    std::size_t hop = 273;
    // Nonzero numerator selects rational live-equivalent cadence; zero uses explicit hop.
    std::uint64_t interval_numerator = 1, interval_denominator = 120;
};
struct CompositionStatistics {
    std::string source;
    std::uint64_t frames = 0, active_frames = 0, samples = 0;
    double rms = 0;
    std::vector<double> mean_spectrum;
    std::vector<double> frequency_presence;
};
struct FrequencyProfile {
    ProfileCompatibility compatibility;
    ProfileTraining training;
    std::vector<float> gains;
    std::vector<double> mean_spectrum;
    std::vector<double> frequency_presence;
    std::vector<CompositionStatistics> compositions;
};
enum class ProfileMatchScope { Exact, SignalOnly };
enum class ProfileMismatchReason {
    InvalidSettings, InvalidCadence, SampleRate, FftSize, Window,
    CoherentGain, PowerGain, Projection, DcLayout, NyquistLayout,
    ChannelPolicy, ChannelIndex, ResamplerMode, ResamplerQuality,
    ResamplerDesignVersion, Reference, Floor, FrequencyGridLength,
    FrequencyPresence, InvalidCoefficient
};
// Cold validation API; callers translate reasons instead of reimplementing matching.
std::vector<ProfileMismatchReason> profile_mismatches(const FrequencyProfile&,
    const ProfileCompatibility&,ProfileMatchScope scope=ProfileMatchScope::Exact);
WindowScaling profile_window_scaling(const ProfileCompatibility&) noexcept;
bool valid_profile_settings(const ProfileCompatibility&, const ProfileTraining&, std::string& error);
bool validate_profile(const FrequencyProfile&, const ProfileCompatibility& expected, std::string& error);
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
