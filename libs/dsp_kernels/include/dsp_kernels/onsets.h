#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace dsp_kernels {
inline constexpr std::size_t max_onset_features=4096, max_onset_channels=8;
inline constexpr std::size_t onset_noise_frames=64;
struct OnsetBand {
    unsigned first=0, count=0;
    bool operator==(const OnsetBand&) const=default;
};
struct OnsetConfig {
    unsigned feature_count=64, channel_count=0;
    // Zero channels selects the complete declared feature vector. Ranges are
    // explicit feature indices, not inferred instruments or independent votes.
    std::array<OnsetBand,max_onset_channels> channels{};
    float contrast_floor=.08f,noise_multiplier=3,min_positive_mass=8.f/255.f;
    double refractory_s=.085;
    bool operator==(const OnsetConfig&) const=default;
};
struct OnsetPeak {
    double t_obs=0,t_ready=0;
    float strength=0,positive_mass=0;
};
struct OnsetObservation {
    float flux=0,contrast=0,threshold=0,mean_level=0;
    unsigned above_unit_values=0;
    std::optional<OnsetPeak> peak;
    std::optional<double> pending_t_obs; // Earliest possible future confirmation.
};
// Causal positive spectral flux on linear nonnegative amplitudes. Float input
// retains headroom above one; UInt8 is explicitly scaled by 1/255. Strength is
// a bounded contrast, never a calibrated probability or an instrument label.
// A peak is delivered on its first falling frame, with its original time and
// the current readiness time. A plateau keeps its earliest maximum.
// Fixed storage, no allocation in prepare/process/reset. Invalid operations
// preserve state and output. A source discontinuity requires an explicit reset.
// Analytics/noise/strength are Float32; event/readiness times remain Double.
// An unrepresentable Float32 reduction/threshold is rejected before detection.
class PreparedOnsets {
public:
    bool prepare(const OnsetConfig& config) noexcept;
    void reset() noexcept;
    bool process(std::span<const float> values,double t_obs,double t_ready) noexcept;
    bool process(std::span<const std::uint8_t> values,double t_obs,double t_ready) noexcept;
    std::span<const OnsetObservation> output() const noexcept;
    const OnsetConfig& config() const noexcept {return config_;}
    static std::size_t workspace_bytes() noexcept;
private:
    struct Input {
        std::span<const float> floats;
        std::span<const std::uint8_t> bytes;
        std::size_t size() const noexcept;
        float value(std::size_t index) const noexcept;
    };
    struct Channel {
        std::array<float,onset_noise_frames> noise{};
        unsigned noise_count=0, noise_write=0;
        float previous_contrast=0;
        std::optional<OnsetPeak> pending;
        std::optional<double> last_event;
    };
    struct Measurement {
        OnsetObservation observation;
        float positive_mass=0;
    };
    bool valid_input(const Input& input,double observed,double ready) const noexcept;
    bool process_input(const Input& input,double observed,double ready) noexcept;
    OnsetBand band(unsigned channel) const noexcept;
    bool measure(OnsetBand range,Measurement&) const noexcept;
    bool measure_channel(unsigned,Measurement&) const noexcept;
    void publish_measurements(std::span<const Measurement>,double,double) noexcept;
    bool measure_input(const Input&,std::span<Measurement>) noexcept;
    void detect(unsigned channel,OnsetObservation& value,float positive_mass,double observed,double ready) noexcept;
    OnsetConfig config_;
    std::array<float,max_onset_features> previous_{};
    std::array<float,max_onset_features> current_{},differences_{};
    std::array<Channel,max_onset_channels> channels_{};
    std::array<OnsetObservation,max_onset_channels> output_{};
    unsigned channel_count_=0;
    std::optional<double> last_time_;
};
}
