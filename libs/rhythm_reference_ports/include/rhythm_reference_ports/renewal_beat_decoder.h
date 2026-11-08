// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct RenewalBeatConfig {
    float frame_rate=44100.f/512.f,maximum_period_s=1.5f,sigma_ibi=.025f,transition_mix=.5f;
    unsigned upsampling=1;
};
// Source port of Degara's reliability-informed event-state decoder. Explicit
// onset curve, period seconds and period-block end times. No hidden FFT, ODF
// resampling, tempo estimation or global clocks. Output is an offline beat mask.
class PreparedRenewalBeatDecoder {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_samples,std::size_t maximum_blocks,RenewalBeatConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_samples,std::size_t maximum_period_blocks,RenewalBeatConfig={});
    bool process(std::span<const float> onset,std::span<const float> periods_s,
                 std::span<const float> block_ends_s,std::span<std::uint8_t> beats) noexcept;
    std::span<const std::uint32_t> state_path() const noexcept {return std::span(path_).first(result_samples_);}
private:
    bool configure(std::size_t,std::size_t,RenewalBeatConfig);
    bool prepare_model_storage();
    void allocate(std::size_t gaussian_radius);
    bool state_count(float maximum_period,std::size_t& count) const noexcept;
    void gaussian() noexcept;
    bool input(std::span<const float>,std::span<const float>,std::span<const float>,std::span<std::uint8_t>) const noexcept;
    bool models(std::span<const float>) noexcept;
    void model(float period,std::size_t index) noexcept;
    bool normalize(std::span<const float>) noexcept;
    bool decode(std::span<const float> periods,std::span<const float> ends,std::size_t samples) noexcept;
    void advance(float activation,std::size_t model,std::size_t frame) noexcept;
    std::uint32_t reset_source(std::size_t model,float& cost) const noexcept;
    bool backtrack(std::size_t samples) noexcept;
    RenewalBeatConfig config_;
    std::size_t maximum_samples_=0,maximum_blocks_=0,maximum_states_=0,states_=0,result_samples_=0;
    float rate_=0,resolution_=0;
    std::vector<float> gaussian_,reset_log_,advance_log_,previous_,current_,onset_;
    std::vector<std::uint32_t> reset_from_,path_;
    std::vector<std::uint8_t> beats_;
};
}
