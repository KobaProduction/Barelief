#pragma once
#include <dsp_kernels/onset_strength.h>
namespace dsp_kernels {
enum class FeatureReduction {None,Mean,Median,Sum};
struct FeatureReductionConfig {
    FeatureReduction operation=FeatureReduction::None;
    bool custom=false;
};
// Shared source reduction, preserving nonfinite results explicitly when asked.
bool reduce_feature_values(std::span<float>,FeatureReduction,float&,bool allow_nonfinite=false) noexcept;
// Reduces the time/row axis of computed features. Aggregated native modes retain
// only the computed feature matrix, not their input spectrogram/tempogram;
// raw modes stream rows. Storage is prepared; no allocation in process.
class PreparedFeatureReduction {
public:
    static bool storage_bytes(std::size_t rows,std::size_t columns,FeatureReductionConfig,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept;
    bool prepare(std::size_t maximum_rows,std::size_t columns,FeatureReductionConfig={});
    bool process(FloatMatrixSource,std::size_t rows,FloatMatrixDestination,FloatMatrixReductionPort={}) noexcept;
    std::size_t output_rows(std::size_t rows) const noexcept;
private:
    bool collect(FloatMatrixSource,std::size_t rows) noexcept;
    bool reduce(std::size_t rows) noexcept;
    std::size_t maximum_rows_=0,columns_=0;
    FeatureReductionConfig config_;
    std::vector<float> matrix_,row_,column_,result_;
};
}
