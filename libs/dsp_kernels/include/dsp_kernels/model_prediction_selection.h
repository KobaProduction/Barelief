#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <cstddef>
#include <limits>
#include <span>
#include <vector>

namespace dsp_kernels {
// Classical madmom MultiModelSelection postprocessor, supplied predictions only.
// Rows are [model][flattened value]. reference_count=0 averages all models and
// compares all; otherwise average the first min(reference_count,models), then
// compare the remainder. Initial best SSE is the VALUE COUNT, with strict <:
// all candidates above/equal to that bound correctly yield an EMPTY selection.
// No inference, weights or forced [0,1] clipping. Source first-tie order retained.
class PreparedModelPredictionSelection {
public:
    static bool storage_bytes(std::size_t models,std::size_t values,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_models,std::size_t maximum_values);
    bool process(FloatMatrixSource,std::size_t models,std::size_t values,std::size_t reference_count=0) noexcept;
    std::size_t selected_model() const noexcept {return ready_?selected_:std::numeric_limits<std::size_t>::max();}
    std::span<const float> selected_values() const noexcept;
    std::span<const float> reference() const noexcept;
    std::span<const float> candidate_errors() const noexcept;
private:
    bool read(FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool average(FloatMatrixSource,std::size_t,std::size_t) noexcept;
    bool select(FloatMatrixSource,std::size_t,std::size_t,std::size_t) noexcept;
    std::size_t maximum_models_=0,value_count_=0,model_count_=0,candidate_start_=0;
    std::size_t selected_=std::numeric_limits<std::size_t>::max();
    bool ready_=false;
    std::vector<float> reference_,row_,result_,errors_,ones_;
};
}
