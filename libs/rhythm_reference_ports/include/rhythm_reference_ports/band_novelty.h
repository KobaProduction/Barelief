// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/causal_average.h>
#include <rhythm_reference_ports/novelty_weights.h>
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct BandNoveltyConfig {
    float frame_rate=44100.f/128.f;
    NoveltyWeight weighting=NoveltyWeight::Hybrid;
    bool normalize_bands=false;
};
// Source NoveltyCurve. Rows are chronological [frame][band] linear values.
// It returns frames-1 samples: derivative sample i corresponds to input i+1.
// Local-mean subtraction is deliberately sequential/in-place as in the source,
// followed by per-band FIR, weighted mix (or four-curve product) and final FIR.
// Reads each input row once and stores derived novelty, never an input-history
// clone. Short windows clip the source's otherwise invalid mean range; a single
// frame gives an empty derivative. These safety repairs are explicit.
class PreparedBandNovelty {
public:
    // Owned dynamic payload; nested cold temporaries included by each query.
    static bool storage_bytes(std::size_t maximum_frames,std::size_t bands,BandNoveltyConfig,std::span<const float> weights,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_frames,std::size_t bands,BandNoveltyConfig={},
                 std::span<const float> supplied_weights={});
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,std::span<float> novelty) noexcept;
    std::span<const float> band_novelty(std::size_t band) const noexcept;
    std::size_t smoothing_length() const noexcept {return mean_size_;}
    std::span<const float> weights() const noexcept {return weights_;}
private:
    bool configure(std::size_t,std::size_t,BandNoveltyConfig,std::span<const float>);
    bool prepare_weights(std::span<const float>);
    bool derivatives(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    void store_difference(std::size_t) noexcept;
    bool process_band(std::size_t,std::size_t) noexcept;
    void remove_local_mean(std::span<float>) const noexcept;
    float local_mean(std::span<const float>,std::size_t) const noexcept;
    float mix_frame(std::size_t) const noexcept;
    bool mix(std::size_t) noexcept;
    BandNoveltyConfig config_;
    PreparedCausalAverage average_;
    std::size_t maximum_frames_=0,bands_=0,mean_size_=0,active_=0;
    std::vector<float> row_,current_,previous_,derived_,weights_,mixed_;
};
}
