// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <rhythm_reference_ports/causal_average.h>
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
// Edge-padded FIR mean subtraction and half-wave rectification. The convenience
// overload uses a fresh centered 2*half+1 average. The explicit filter overload
// retains the supplied FIR length/state: Degara reuses one filter even when
// the padding/alignment half-window changes between full ODF and ACF stages.
class PreparedMovingThreshold {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_values,std::size_t half_window,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_values,std::size_t half_window);
    bool process(std::span<const float>,std::span<float>) noexcept;
    bool process(std::span<const float>,std::span<float>,PreparedCausalAverage&) noexcept;
private:
    float padded(std::span<const float>,std::size_t) const noexcept;
    PreparedCausalAverage average_;
    std::size_t maximum_values_=0,half_=0;
    std::vector<float> values_;
};
class PreparedPeriodPath {
public:
    // Dynamic owned/cold-preparation upper bound; excludes this object.
    static bool storage_bytes(std::size_t maximum_frames,PeriodPathConfig,std::size_t& bytes) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,PeriodPathConfig={});
    // Input rows are ODF frames AFTER the explicit full-series threshold.
    // Padding/cutting is an explicit upstream operation, not inferred here.
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,
                 std::span<float> period_seconds,std::span<float> end_seconds) noexcept;
    // Full Degara composition supplies the same FIR already advanced by its
    // full-ODF threshold. The overload above starts with a fresh filter state.
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,
                 std::span<float> period_seconds,std::span<float> end_seconds,PreparedCausalAverage&) noexcept;
    // Borrowed diagnostics; caller uses only the successfully processed row prefix.
    std::span<const float> observations() const noexcept {return observations_;}
    std::span<const std::uint32_t> decoded_path() const noexcept {return path_;}
    std::span<const float> last_correlation() const noexcept {return acf_;}
    std::span<const float> last_comb() const noexcept {return comb_;}
    std::span<const float> last_filtered() const noexcept {return filtered_;}
    std::size_t frame_length() const noexcept {return config_.frame_length;}
    std::size_t hop() const noexcept {return config_.hop;}
private:
    bool configure(std::size_t,PeriodPathConfig);
    void allocate();
    bool prepare_average();
    void prior() noexcept;
    void transitions() noexcept;
    bool observations(dsp_kernels::FloatMatrixSource,std::size_t,PreparedCausalAverage&) noexcept;
    bool comb() noexcept;
    bool observation(std::size_t frame,PreparedCausalAverage&) noexcept;
    bool noise(std::size_t frames) noexcept;
    bool decode(std::size_t frames) noexcept;
    bool advance(std::size_t frame) noexcept;
    PeriodPathConfig config_;
    std::size_t maximum_frames_=0,periods_=0,min_period_=0,max_period_=0,minimum_user_=0,maximum_user_=0;
    float observations_max_=0;
    PreparedMovingThreshold threshold_;
    PreparedCausalAverage average_;
    std::vector<float> frame_,acf_,comb_,filtered_,weights_,transition_,observations_,previous_,current_;
    std::vector<std::uint32_t> predecessor_,path_;
};
}
