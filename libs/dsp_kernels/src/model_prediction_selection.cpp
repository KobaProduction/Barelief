// Adapted from madmom features/beats.py MultiModelSelectionProcessor at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/model_prediction_selection.h>
#include <dsp_kernels/filter_projection.h>
#include <algorithm>
#include <cmath>
#include <utility>

namespace dsp_kernels {
bool PreparedModelPredictionSelection::prepare(std::size_t models,std::size_t values) {
    std::size_t required=0;if(!storage_bytes(models,values,required))return false;
    if(!models || !values)return false;
    PreparedModelPredictionSelection next;next.maximum_models_=models;
    next.reference_.resize(values);next.row_.resize(values);next.result_.resize(values);
    next.ones_.assign(values,1.f);next.errors_.resize(models);if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedModelPredictionSelection::read(FloatMatrixSource source,std::size_t model,std::size_t count) noexcept {
    auto row=std::span(row_).first(count);
    return source.read(source.context,model,row) &&
        std::all_of(row.begin(),row.end(),[](float x){return std::isfinite(x);});
}
void PreparedModelPredictionSelection::add_reference_row(std::size_t values) noexcept {
    for(std::size_t i=0;i<values;++i)reference_[i]+=row_[i];
}
bool PreparedModelPredictionSelection::candidate_error(std::size_t values,float& error) noexcept {
    for(std::size_t i=0;i<values;++i)result_[i]=row_[i]-reference_[i];
    return project_filter_weights(std::span(result_).first(values),std::span(ones_).first(values),error,{false,true});
}
bool PreparedModelPredictionSelection::publish_selection(FloatMatrixSource source,std::size_t values) noexcept {
    if(selected_==std::numeric_limits<std::size_t>::max())return true;
    if(!read(source,selected_,values))return false;
    std::copy_n(row_.begin(),values,result_.begin());
    return true;
}
bool PreparedModelPredictionSelection::average(FloatMatrixSource source,std::size_t models,std::size_t values) noexcept {
    std::fill_n(reference_.begin(),values,0.f);
    for(std::size_t model=0;model<models;++model){
        if(!read(source,model,values))return false;
        add_reference_row(values);
    }
    for(std::size_t i=0;i<values;++i){
        reference_[i]/=float(models);
        if(!std::isfinite(reference_[i]))return false;
    }
    return true;
}
bool PreparedModelPredictionSelection::select(FloatMatrixSource source,std::size_t begin,std::size_t models,std::size_t values) noexcept {
    float best=float(values);
    for(std::size_t model=begin;model<models;++model){
        if(!read(source,model,values))return false;
        float error=0;
        if(!candidate_error(values,error))return false;
        errors_[model]=error;
        if(error<best){
            selected_=model;best=error;
        }
    }
    return true;
}
bool PreparedModelPredictionSelection::process(FloatMatrixSource source,std::size_t models,std::size_t values,std::size_t refs) noexcept {
    ready_=false;selected_=std::numeric_limits<std::size_t>::max();value_count_=0;
    if(!maximum_models_ || !source.read || !models || models>maximum_models_ || values>row_.size())return false;
    const auto reference_models=refs?std::min(refs,models):models;
    if(!average(source,reference_models,values) || !select(source,std::min(refs,models),models,values))return false;
    if(!publish_selection(source,values))return false;
    value_count_=values;model_count_=models;candidate_start_=std::min(refs,models);ready_=true;return true;
}
std::span<const float> PreparedModelPredictionSelection::selected_values() const noexcept {
    return ready_ && selected_!=std::numeric_limits<std::size_t>::max()?std::span<const float>(result_).first(value_count_):std::span<const float>{};
}
std::span<const float> PreparedModelPredictionSelection::reference() const noexcept {
    return ready_?std::span<const float>(reference_).first(value_count_):std::span<const float>{};
}
std::span<const float> PreparedModelPredictionSelection::candidate_errors() const noexcept {
    return ready_?std::span<const float>(errors_).subspan(candidate_start_,model_count_-candidate_start_):std::span<const float>{};
}
}


namespace dsp_kernels {
bool PreparedModelPredictionSelection::storage_bytes(std::size_t models,std::size_t values,std::size_t& out) noexcept {
    std::size_t bytes=0;
    if(!models || !values || !prepared_storage::add_array(bytes,models,sizeof(float)) ||
       !prepared_storage::add_array(bytes,values,4*sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedModelPredictionSelection::workspace_bytes() const noexcept {
    return (reference_.capacity()+row_.capacity()+result_.capacity()+errors_.capacity()+ones_.capacity())*sizeof(float);
}
}
