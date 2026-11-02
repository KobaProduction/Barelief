#pragma once
#include <dsp_kernels/numeric_rows.h>
#include <vector>

namespace dsp_kernels {
enum class DbReductionAxes { All, Rows, Columns };
enum class DbReference { Value, Maximum };
struct PowerToDbConfig {
    float reference=1,amin=1e-10f,top_db=80;
    bool top_db_enabled=true;
    DbReference reference_mode=DbReference::Value;
    DbReductionAxes axes=DbReductionAxes::All;
};
// Native Float32 power_to_db, including references/floors and arithmetic:
// axes refer to the supplied [row][column] matrix.
// All is the mono librosa axes='auto' case; Rows/Columns reduce those axes.
// Reference/top_db are computed over this call's complete supplied matrix,
// never from future data, previous calls or a running autoscaler. Supports
// scalar and np.max reference; arbitrary Python callable references are external.
// Input reader must be stable for both passes. No allocation/history copies.
// A failed writer may have received earlier rows; graph adapters publish only
// after successful completion. Preparation preserves the old plan on failure.
class PreparedPowerToDb {
public:
    bool prepare(std::size_t maximum_rows,std::size_t columns,const PowerToDbConfig& config={});
    bool process(FloatMatrixSource,std::size_t rows,std::size_t columns,FloatMatrixDestination) noexcept;
    // Two-pass readers can retain only the reduction scales, then transform
    // randomly accessed rows. Values must belong to that same measured matrix.
    bool measure_reference(FloatMatrixSource,std::size_t rows,std::size_t columns) noexcept;
    bool process_row(std::size_t row,std::span<const float> input,std::span<float> output) const noexcept;
    // Total includes sizeof(PreparedPowerToDb); workspace_bytes is dynamic only.
    static bool storage_bytes(std::size_t maximum_rows,std::size_t columns,const PowerToDbConfig&,std::size_t&) noexcept;
    std::size_t workspace_bytes() const noexcept {
        return (row_.capacity()+result_.capacity()+peaks_.capacity()+floors_.capacity()+references_.capacity())*sizeof(float);
    }
private:
    std::size_t group(std::size_t row,std::size_t column) const noexcept;
    bool measure(FloatMatrixSource,std::size_t rows) noexcept;
    bool transform(FloatMatrixSource,std::size_t rows,FloatMatrixDestination) noexcept;
    float db(float value,std::size_t group) const noexcept;
    void prepare_scales() noexcept;
    PowerToDbConfig config_;
    std::size_t maximum_rows_=0,maximum_columns_=0,columns_=0,measured_rows_=0;
    bool scales_ready_=false;
    std::vector<float> row_,result_,peaks_,floors_,references_;
};
}
