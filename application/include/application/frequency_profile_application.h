#pragma once
#include <domain/processing_contracts.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace application {
struct ProfileFrontend {
    double sample_rate=32768, coherent_gain=0, power_gain=0;
    std::size_t fft_size=4096;
    bool include_dc=true, include_nyquist=true, normalize=true;
    std::string window_function="hann", representation="power", channel_policy="mean";
    std::uint32_t channel_index=0;
    std::string resampler_mode="existing_fir";
    unsigned resampler_quality=5;
    bool operator==(const ProfileFrontend&) const = default;
};
struct ProfileTrainingCadence {
    std::uint64_t interval_numerator=1, interval_denominator=120;
    bool operator==(const ProfileTrainingCadence&) const = default;
};
struct ProfileRoute {
    std::string graph_id;
    domain::NodeId coefficients=-1, arithmetic=-1, frame_source=-1;
    domain::PortId coefficient_output=0, coefficient_input=0;
    domain::PortId frame_output=0, frame_input=0;
    std::size_t gain_first_bin=1, gain_count=0;
    ProfileFrontend frontend;
    ProfileTrainingCadence training;
};
enum class ProfileMismatchReason {
    InvalidSettings, InvalidCadence, SampleRate, FftSize, Window,
    CoherentGain, PowerGain, Projection, DcLayout, NyquistLayout,
    ChannelPolicy, ChannelIndex, ResamplerMode, ResamplerQuality,
    ResamplerDesignVersion, Reference, Floor, FrequencyGridLength,
    FrequencyPresence, InvalidCoefficient
};
// Caller supplies the concrete profile math adapter. Its methods are synchronous;
// it validates the same gains span that will be staged, returns at most one
// reason per enum value, and never mutates the graph.
class ProfileMatcher {
public:
    virtual ~ProfileMatcher()=default;
    virtual bool valid_declaration(const ProfileRoute&) const=0;
    virtual std::vector<ProfileMismatchReason> mismatches(const ProfileRoute&,std::span<const float> gains) const=0;
};
enum class ProfileApplyRefusal { None, RouteUnavailable, SharedCoefficients,
    ProfileMismatch, CoefficientRange };
struct ProfileApplyResult {
    ProfileApplyRefusal refusal=ProfileApplyRefusal::None;
    std::vector<ProfileMismatchReason> mismatches;
    ProfileFrontend expected_frontend;
    domain::ConfiguredProcessingNode coefficients, arithmetic;
    explicit operator bool() const noexcept { return refusal==ProfileApplyRefusal::None; }
};

std::vector<ProfileRoute> frequency_profile_routes(const domain::NumericProcessingGraph&,
    const ProfileMatcher&);
// gains is borrowed only during this call; the returned node owns its copied slice.
ProfileApplyResult apply_frequency_profile(const domain::NumericProcessingGraph&,
    const ProfileRoute&,std::span<const float> gains,const ProfileMatcher&);
}
