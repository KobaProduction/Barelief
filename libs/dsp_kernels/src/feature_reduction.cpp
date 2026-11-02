#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/feature_reduction.h>
#include <algorithm>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool reduce_feature_values(std::span<float> values,FeatureReduction method,float& output,bool nonfinite) noexcept {
    if(method==FeatureReduction::None || unsigned(method)>unsigned(FeatureReduction::Sum))return false;
    const auto mode=method==FeatureReduction::Mean?OnsetAggregate::Mean:
        method==FeatureReduction::Median?OnsetAggregate::Median:OnsetAggregate::Sum;
    return aggregate_onset_values(values,mode,output,nonfinite);
}
bool PreparedFeatureReduction::prepare(std::size_t rows,std::size_t columns,FeatureReductionConfig c) {
    std::size_t required=0;if(!storage_bytes(rows,columns,c,required))return false;
    if(!rows || columns>SIZE_MAX/rows || unsigned(c.operation)>unsigned(FeatureReduction::Sum))return false;
    PreparedFeatureReduction next;next.maximum_rows_=rows;next.columns_=columns;next.config_=c;
    next.row_.resize(columns);next.column_.resize(rows);next.result_.resize(columns);
    if(!c.custom && c.operation!=FeatureReduction::None)next.matrix_.resize(rows*columns);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
std::size_t PreparedFeatureReduction::output_rows(std::size_t rows) const noexcept {
    if(!maximum_rows_ || !rows || rows>maximum_rows_)return 0;
    return config_.custom || config_.operation!=FeatureReduction::None?1:rows;
}
bool PreparedFeatureReduction::collect(FloatMatrixSource source,std::size_t rows) noexcept {
    for(std::size_t time=0;time<rows;++time)
        if(!source.read(source.context,time,std::span(matrix_).subspan(time*columns_,columns_)))return false;
    return true;
}
bool PreparedFeatureReduction::reduce(std::size_t rows) noexcept {
    for(std::size_t feature=0;feature<columns_;++feature){
        for(std::size_t time=0;time<rows;++time)column_[time]=matrix_[time*columns_+feature];
        if(!reduce_feature_values(std::span(column_).first(rows),config_.operation,result_[feature],true))return false;
    }
    return true;
}
bool PreparedFeatureReduction::process(FloatMatrixSource source,std::size_t rows,FloatMatrixDestination output,FloatMatrixReductionPort custom) noexcept {
    if(!output_rows(rows) || !source.read || !output.write || bool(custom.reduce)!=config_.custom)return false;
    if(config_.custom)
        return custom.reduce && custom.reduce(custom.context,source,rows,columns_,result_) && output.write(output.context,0,result_);
    if(config_.operation!=FeatureReduction::None)
        return collect(source,rows) && reduce(rows) && output.write(output.context,0,result_);
    for(std::size_t time=0;time<rows;++time)
        if(!source.read(source.context,time,row_) || !output.write(output.context,time,row_))return false;
    return true;
}
}


namespace dsp_kernels {
bool PreparedFeatureReduction::storage_bytes(std::size_t rows,std::size_t cols,FeatureReductionConfig c,std::size_t& out) noexcept {
    std::size_t bytes=0,count=0;
    if(!rows || !prepared_storage::mul_bytes(rows,cols,count) || unsigned(c.operation)>unsigned(FeatureReduction::Sum) ||
       !prepared_storage::add_array(bytes,cols,2*sizeof(float)) || !prepared_storage::add_array(bytes,rows,sizeof(float)))return false;
    if(!c.custom && c.operation!=FeatureReduction::None && !prepared_storage::add_array(bytes,count,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedFeatureReduction::workspace_bytes() const noexcept {
    return (matrix_.capacity()+row_.capacity()+column_.capacity()+result_.capacity())*sizeof(float);
}
}
