#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <dsp_kernels/onset_strength.h>
#include <rhythm_reference_ports/scalar_history.h>
#include <cstddef>
#include <span>
#include <vector>
namespace rhythm_reference_ports {
struct SpectralDifferenceSequenceConfig {
    std::size_t lag=1,frequency_maximum=1;
    bool positive=false;
};
// SpectrogramDifferenceProcessor: source first-chunk-sized rolling buffer,
// cached lag retained across reset, maximum reference and signed/positive modes.
// Initial infinite context and infinite differences become zero like source.
// Subsequent chunks emit FIRST-chunk frame count, even for unequal chunk sizes.
// Difference and current-feature ports are explicit. Source stack_diffs is
// composition with existing concat/stack operations, not a hidden callback.
// Reuses ScalarHistory and shared numerical differences, no processing growth
// or full-history copies. Empty chunks safely emit nothing. Failed input/writer
// may consume state/output prefix; reset before retrying an independent batch.
class PreparedSpectralDifferenceSequence {
public:
    bool prepare(std::size_t maximum_chunk_frames,std::size_t features,SpectralDifferenceSequenceConfig={});
    static bool storage_bytes(std::size_t maximum_chunk_frames,std::size_t features,SpectralDifferenceSequenceConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {return history_.workspace_bytes()+difference_.workspace_bytes()+(row_.capacity()+differences_.capacity())*sizeof(float);}
    void reset() noexcept;
    bool process(dsp_kernels::FloatMatrixSource,std::size_t frames,dsp_kernels::FloatMatrixDestination differences,
                 bool reset_state=true,dsp_kernels::FloatMatrixDestination current_features={}) noexcept;
    std::size_t output_frames() const noexcept {return output_frames_;}
private:
    bool configure(std::size_t,std::size_t,SpectralDifferenceSequenceConfig);
    bool initialize(std::size_t) noexcept;
    bool append(dsp_kernels::FloatMatrixSource,std::size_t) noexcept;
    bool emit(dsp_kernels::FloatMatrixDestination,dsp_kernels::FloatMatrixDestination) noexcept;
    bool difference(std::span<const float>,std::span<const float>) noexcept;
    SpectralDifferenceSequenceConfig config_;
    ScalarHistory history_;
    dsp_kernels::PreparedOnsetStrength difference_;
    std::vector<float> row_,differences_;
    std::size_t maximum_chunk_=0,features_=0,buffer_frames_=0,output_frames_=0;
    bool started_=false;
};
}
