// Adapted from librosa 1.0.0 core.spectrum.power_to_db (Float32 real S).
// ISC license: packaging/licenses/librosa.txt; included in distributions/SDK.
// Copyright (c) 2013--2026, librosa development team.
// https://github.com/librosa/librosa/blob/1.0.0/librosa/core/spectrum.py
#include <dsp_kernels/power_to_db.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace dsp_kernels {
namespace {
bool valid(const PowerToDbConfig& c) noexcept {
    return std::isfinite(c.reference) && std::isfinite(c.amin) && c.amin>0 &&
        std::isfinite(c.top_db) && c.top_db>=0 &&
        unsigned(c.reference_mode)<=unsigned(DbReference::Maximum) && unsigned(c.axes)<=unsigned(DbReductionAxes::Columns);
}
float power_db(float value,float minimum) noexcept {return 10.f*std::log10(std::max(minimum,value));}
bool append_bytes(std::size_t count,std::size_t width,std::size_t& bytes) noexcept {
    if(count>(SIZE_MAX-bytes)/width)return false;
    bytes+=count*width;return true;
}
}
bool PreparedPowerToDb::storage_bytes(std::size_t rows,std::size_t columns,const PowerToDbConfig& c,std::size_t& out) noexcept {
    if(!rows || !columns || !valid(c))return false;
    const auto groups=c.axes==DbReductionAxes::Rows?columns:c.axes==DbReductionAxes::Columns?rows:1;
    std::size_t bytes=sizeof(PreparedPowerToDb);
    if(!append_bytes(columns,2*sizeof(float),bytes) || !append_bytes(groups,3*sizeof(float),bytes))return false;
    out=bytes;return true;
}
bool PreparedPowerToDb::prepare(std::size_t rows,std::size_t columns,const PowerToDbConfig& config) {
    std::size_t bytes=0;if(!storage_bytes(rows,columns,config,bytes))return false;
    PreparedPowerToDb next;next.config_=config;next.maximum_rows_=rows;next.maximum_columns_=columns;
    const auto groups=config.axes==DbReductionAxes::Rows?columns:config.axes==DbReductionAxes::Columns?rows:1;
    next.row_.resize(columns);next.result_.resize(columns);next.peaks_.resize(groups);next.floors_.resize(groups);next.references_.resize(groups);
    if(next.workspace_bytes()>bytes-sizeof(PreparedPowerToDb))return false;
    *this=std::move(next);return true;
}
std::size_t PreparedPowerToDb::group(std::size_t row,std::size_t column) const noexcept {
    return config_.axes==DbReductionAxes::Rows?column:config_.axes==DbReductionAxes::Columns?row:0;
}
bool PreparedPowerToDb::measure(FloatMatrixSource source,std::size_t rows) noexcept {
    std::fill(peaks_.begin(),peaks_.end(),-std::numeric_limits<float>::infinity());
    for(std::size_t row=0;row<rows;++row){
        if(!source.read(source.context,row,std::span(row_).first(columns_)))return false;
        for(std::size_t column=0;column<columns_;++column){
            if(!std::isfinite(row_[column]))return false;
            auto& peak=peaks_[group(row,column)];peak=std::max(peak,row_[column]);
        }
    }
    return true;
}
float PreparedPowerToDb::db(float value,std::size_t at) const noexcept {
    return power_db(value,config_.amin)-references_[at];
}
void PreparedPowerToDb::prepare_scales() noexcept {
    for(std::size_t at=0;at<peaks_.size();++at){
        references_[at]=config_.reference_mode==DbReference::Maximum?power_db(peaks_[at],config_.amin):
            power_db(std::abs(config_.reference),config_.amin);
        floors_[at]=config_.top_db_enabled?db(peaks_[at],at)-config_.top_db:-std::numeric_limits<float>::infinity();
    }
}
bool PreparedPowerToDb::transform(FloatMatrixSource source,std::size_t rows,FloatMatrixDestination destination) noexcept {
    for(std::size_t row=0;row<rows;++row){
        if(!source.read(source.context,row,std::span(row_).first(columns_)))return false;
        if(!process_row(row,std::span(row_).first(columns_),std::span(result_).first(columns_)))return false;
        if(!destination.write(destination.context,row,std::span(result_).first(columns_)))return false;
    }
    return true;
}
bool PreparedPowerToDb::measure_reference(FloatMatrixSource source,std::size_t rows,std::size_t columns) noexcept {
    scales_ready_=false;measured_rows_=0;
    if(!maximum_columns_ || !rows || rows>maximum_rows_ || !columns || columns>maximum_columns_ || !source.read)return false;
    columns_=columns;
    if(!measure(source,rows))return false;
    prepare_scales();measured_rows_=rows;scales_ready_=true;return true;
}
bool PreparedPowerToDb::process_row(std::size_t row,std::span<const float> input,std::span<float> output) const noexcept {
    if(!scales_ready_ || row>=measured_rows_ || input.size()!=columns_ || output.size()!=columns_ ||
        !std::all_of(input.begin(),input.end(),[](float x){return std::isfinite(x);}))return false;
    for(std::size_t column=0;column<columns_;++column){
        const auto at=group(row,column);output[column]=std::max(db(input[column],at),floors_[at]);
    }
    return true;
}
bool PreparedPowerToDb::process(FloatMatrixSource source,std::size_t rows,std::size_t columns,FloatMatrixDestination destination) noexcept {
    if(!maximum_columns_ || rows>maximum_rows_ || columns>maximum_columns_ || !source.read || !destination.write)return false;
    if(!rows || !columns)return !config_.top_db_enabled && config_.reference_mode==DbReference::Value;
    return measure_reference(source,rows,columns) && transform(source,rows,destination);
}
}
