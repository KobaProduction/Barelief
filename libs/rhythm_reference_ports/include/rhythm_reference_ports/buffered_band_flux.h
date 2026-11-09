// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <rhythm_reference_ports/buffered_maximum.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/numeric_rows.h>
#include <span>
#include <vector>

namespace rhythm_reference_ports {
struct BufferedBandFluxConfig {std::size_t bin_width=3,frame_width=2;};
// Source SuperFluxNovelty: persistent noncausal MaxFilter over each delayed
// frequency row, then positive differences via the shared onset operation.
// Standard process sums ALL [frame][band] differences into one scalar with
// source accumulation order. A streaming source window has frame_width+1 rows.
// Reset normally retains the child filter as in source SuperFluxNovelty::reset;
// clear_filter explicitly resets it. Reader spans/callbacks are never retained.
class PreparedBufferedBandFlux {
public:
    bool prepare(std::size_t bands,std::size_t maximum_frames,BufferedBandFluxConfig={});
    static bool storage_bytes(std::size_t bands,std::size_t maximum_frames,BufferedBandFluxConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    // Last successfully consumed pair; synchronous until next process/reset.
    std::span<const float> filtered_reference() const noexcept {return reference_;}
    std::span<const float> positive_differences() const noexcept {return differences_;}
    void reset(bool clear_filter=false,BufferedMaximumReset=BufferedMaximumReset::SourceReset) noexcept;
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,float& novelty) noexcept;
    bool process_pair(std::span<const float> current,std::span<const float> delayed,float& novelty) noexcept;
private:
    void allocate(std::size_t,std::size_t,BufferedBandFluxConfig);
    bool accumulate(std::span<const float>,std::span<const float>,float&) noexcept;
    std::size_t maximum_frames_=0;
    BufferedBandFluxConfig config_;
    PreparedBufferedMaximum maximum_;
    dsp_kernels::PreparedOnsetStrength difference_;
    std::vector<float> current_,delayed_,reference_,differences_;
};
}
