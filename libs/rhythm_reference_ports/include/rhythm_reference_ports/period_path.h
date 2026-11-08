// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <rhythm_reference_ports/uniform_noise.h>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct PeriodPathConfig {
    float frame_rate=44100.f/512.f,frame_duration_s=5.944308390022676f,min_bpm=40,max_bpm=208,noise_amount=.0001f;
    std::size_t frame_length=0,hop=0;
    UniformNoisePort noise;
};
class PreparedPeriodPath {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_frames,PeriodPathConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,PeriodPathConfig={});
    // Each row is an explicit normalized period observation vector, width=hop.
    // Correlation, comb weighting, thresholds and bounds are upstream graph nodes.
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,
                 std::span<float> period_seconds,std::span<float> end_seconds) noexcept;
    // Borrowed diagnostics; caller uses only the successfully processed row prefix.
    std::span<const float> observations() const noexcept {return observations_;}
    std::span<const std::uint32_t> decoded_path() const noexcept {return path_;}
    std::size_t frame_length() const noexcept {return config_.frame_length;}
    std::size_t hop() const noexcept {return config_.hop;}
private:
    bool configure(std::size_t,PeriodPathConfig);
    void allocate();
    bool prior() noexcept;
    void transitions() noexcept;
    bool observations(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool noise(std::size_t frames) noexcept;
    bool decode(std::size_t frames) noexcept;
    bool advance(std::size_t frame) noexcept;
    float transition_maximum(std::size_t target,std::uint32_t& source) const noexcept;
    void backtrack(std::size_t frames) noexcept;
    PeriodPathConfig config_;
    std::size_t maximum_frames_=0,periods_=0;
    float observations_max_=0;
    std::vector<float> filtered_,weights_,transition_,observations_,previous_,current_;
    std::vector<std::uint32_t> predecessor_,path_;
};
}
