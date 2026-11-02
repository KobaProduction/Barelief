// SPDX-License-Identifier: ISC
// librosa onset.py onset_strength_multi and util.sync composition,
// librosa/librosa@3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full license: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/onset_strength_sequence.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <utility>
namespace dsp_kernels {
bool PreparedOnsetStrengthSequence::prepare(std::size_t features,std::size_t frames,
    OnsetStrengthSequenceConfig c,std::span<const OnsetFeatureGroup> groups,OnsetAggregationPort reducer) {
    std::size_t required=0;if(!storage_bytes(features,frames,c,groups,required))return false;
    PreparedOnsetStrengthSequence next;
    if(!next.configure(features,frames,c,groups,reducer))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedOnsetStrengthSequence::prepare_groups(std::span<const OnsetFeatureGroup> supplied) {
    if(supplied.empty()){
        groups_.resize(1);groups_[0].resize(features_);
        std::iota(groups_[0].begin(),groups_[0].end(),std::size_t{0});
    } else {
        groups_.reserve(supplied.size());
        for(const auto& group:supplied){
            if(group.indices.empty() || group.indices.size()>features_ ||
                std::any_of(group.indices.begin(),group.indices.end(),[this](std::size_t i){return i>=features_;}))return false;
            groups_.emplace_back(group.indices.begin(),group.indices.end());
        }
    }
    detrend_.resize(groups_.size());output_.resize(groups_.size());
    constexpr std::array b{1.f,-1.f},a{1.f,-.99f};
    for(auto& filter:detrend_)if(!filter.prepare(b,a,SubnormalStatePolicy::Preserve))return false;
    return true;
}
bool PreparedOnsetStrengthSequence::configure(std::size_t features,std::size_t frames,
    OnsetStrengthSequenceConfig c,std::span<const OnsetFeatureGroup> groups,OnsetAggregationPort reducer) {
    config_=c;features_=features;reducer_=reducer;
    auto primitive=c.strength;
    if(c.aggregate_features)primitive.detrend=false;
    if(!difference_.prepare(features,frames,primitive))return false;
    scratch_.resize(features);
    return !c.aggregate_features || prepare_groups(groups);
}
bool PreparedOnsetStrengthSequence::reduce_groups(std::span<const float> values,bool active) noexcept {
    for(std::size_t g=0;g<groups_.size();++g){
        const auto& indices=groups_[g];auto scratch=std::span(scratch_).first(indices.size());
        float value=0;
        if(active){
            for(std::size_t i=0;i<indices.size();++i)scratch[i]=values[indices[i]];
            if(reducer_.reduce){if(!reducer_.reduce(reducer_.context,scratch,value))return false;}
            else if(!aggregate_onset_values(scratch,config_.strength.aggregate,value))return false;
        }
        if(config_.strength.detrend && !detrend_[g].process(value,value))return false;
        if(!std::isfinite(value))return false;
        output_[g]=value;
    }
    return true;
}
bool PreparedOnsetStrengthSequence::write(void* context,std::size_t time,std::span<const float> values) noexcept {
    auto& writer=*static_cast<Writer*>(context);auto& owner=*writer.owner;
    std::size_t source=0;
    const bool active=owner.difference_.source_frame_for_output(time,writer.frames,source);
    return owner.reduce_groups(values,active) && writer.output.write(writer.output.context,time,owner.output_);
}
bool PreparedOnsetStrengthSequence::process(FloatMatrixSource source,std::size_t frames,
    FloatMatrixDestination output,FloatMatrixSource reference) noexcept {
    if(!features_ || !source.read || !output.write)return false;
    for(auto& filter:detrend_)filter.reset();
    Writer writer{this,output,frames};
    const FloatMatrixDestination destination=config_.aggregate_features?FloatMatrixDestination{&writer,write}:output;
    return reference.read?difference_.process_features(source,reference,frames,destination):
        difference_.process_features(source,frames,destination);
}
}


namespace dsp_kernels {
namespace {
bool group_storage(std::size_t features,std::span<const OnsetFeatureGroup> groups,std::size_t& bytes) noexcept {
    std::size_t indices=groups.empty()?features:0;
    for(const auto& g:groups){
        if(g.indices.empty() || g.indices.size()>features ||
           std::any_of(g.indices.begin(),g.indices.end(),[features](auto i){return i>=features;}) ||
           !prepared_storage::add_bytes(indices,g.indices.size()))return false;
    }
    const auto count=groups.empty()?1:groups.size();std::size_t filter=0;
    return PreparedRecursiveFilter::storage_bytes(2,2,{},filter) &&
        prepared_storage::add_array(bytes,indices,sizeof(std::size_t)) &&
        prepared_storage::add_array(bytes,count,sizeof(std::vector<std::size_t>)+sizeof(PreparedRecursiveFilter)+sizeof(float)) &&
        prepared_storage::add_array(bytes,count,filter);
}
}
bool PreparedOnsetStrengthSequence::storage_bytes(std::size_t features,std::size_t frames,OnsetStrengthSequenceConfig c,
    std::span<const OnsetFeatureGroup> groups,std::size_t& out) noexcept {
    auto primitive=c.strength;if(c.aggregate_features)primitive.detrend=false;
    std::size_t bytes=0;
    if(!PreparedOnsetStrength::storage_bytes(features,frames,primitive,bytes))return false;
    bytes-=sizeof(PreparedOnsetStrength);
    if(!prepared_storage::add_array(bytes,features,sizeof(float)) || (c.aggregate_features && !group_storage(features,groups,bytes)))return false;
    out=bytes;return true;
}
std::size_t PreparedOnsetStrengthSequence::workspace_bytes() const noexcept {
    std::size_t bytes=difference_.workspace_bytes()+scratch_.capacity()*sizeof(float)+output_.capacity()*sizeof(float)+
        groups_.capacity()*sizeof(std::vector<std::size_t>)+detrend_.capacity()*sizeof(PreparedRecursiveFilter);
    for(const auto& g:groups_)bytes+=g.capacity()*sizeof(std::size_t);
    for(const auto& f:detrend_)bytes+=f.storage_bytes();
    return bytes;
}
bool PreparedOnsetStrengthSequence::storage_bytes(std::size_t features,std::size_t frames,OnsetStrengthSequenceConfig c,
    std::size_t groups,std::size_t indices,std::size_t& out) noexcept {
    auto primitive=c.strength;if(c.aggregate_features)primitive.detrend=false;
    std::size_t bytes=0,filter=0;
    if(!PreparedOnsetStrength::storage_bytes(features,frames,primitive,bytes))return false;
    bytes-=sizeof(PreparedOnsetStrength);
    if(!prepared_storage::add_array(bytes,features,sizeof(float)))return false;
    if(c.aggregate_features){
        if(!groups){groups=1;indices=features;}
        if(!PreparedRecursiveFilter::storage_bytes(2,2,{},filter) ||
           !prepared_storage::add_array(bytes,indices,sizeof(std::size_t)) ||
           !prepared_storage::add_array(bytes,groups,sizeof(std::vector<std::size_t>)+sizeof(PreparedRecursiveFilter)+sizeof(float)) ||
           !prepared_storage::add_array(bytes,groups,filter))return false;
    }
    out=bytes;return true;
}
}
