// Adapted from madmom features/downbeats.py SyncronizeFeaturesProcessor at
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2014 Department of Computational Perception, JKU Linz
// and Austrian Research Institute for Artificial Intelligence (OFAI), Vienna.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/beat_feature_synchronization.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dsp_kernels {
bool PreparedBeatFeatureSynchronization::prepare(std::size_t features,BeatFeatureSynchronizationConfig config) {
    std::size_t required=0;if(!storage_bytes(features,config,required))return false;
    if(!features || !config.subdivisions || config.subdivisions>std::numeric_limits<std::size_t>::max()/features ||
       !std::isfinite(config.frame_rate) || config.frame_rate<=0 ||
       unsigned(config.empty)>unsigned(EmptySubdivision::Zero))return false;
    PreparedBeatFeatureSynchronization next;next.maximum_features_=features;next.config_=config;
    next.sums_.resize(features*config.subdivisions);next.row_.resize(features);next.counts_.resize(config.subdivisions);
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedBeatFeatureSynchronization::boundaries(std::size_t frames,std::span<const float> beats,std::size_t& start) noexcept {
    if(frames>std::size_t(std::numeric_limits<int>::max()) ||
       !std::all_of(beats.begin(),beats.end(),[](float x){return std::isfinite(x) && x>=0;}) ||
       !std::is_sorted(beats.begin(),beats.end()))return false;
    const float duration=float(frames)/config_.frame_rate;
    retained_beats_=beats.size();
    while(retained_beats_ && beats[retained_beats_-1]>duration)--retained_beats_;
    if(retained_beats_<2){start=0;return true;}
    const float first=std::max(0.f,std::floor((beats[0]-.02f)*config_.frame_rate));
    if(!std::isfinite(first) || first>float(frames))return false;
    start=std::min(frames,std::size_t(first));return true;
}
void PreparedBeatFeatureSynchronization::accumulate_row(std::span<const float> row,std::size_t subdivision) noexcept {
    ++counts_[subdivision];
    for(std::size_t feature=0;feature<row.size();++feature)sums_[subdivision*maximum_features_+feature]+=row[feature];
}
bool PreparedBeatFeatureSynchronization::interval_end(std::span<const float> beats,std::size_t i,std::size_t frames,std::size_t& end) const noexcept {
    const float duration=beats[i+1]-beats[i],offset=std::min(.5f*duration/float(config_.subdivisions),.05f);
    const float terminal=std::floor((beats[i+1]-offset)*config_.frame_rate);
    if(!std::isfinite(terminal) || terminal<0 || terminal>float(frames))return false;
    end=std::min(frames,std::size_t(terminal));
    return true;
}

bool PreparedBeatFeatureSynchronization::interval(FloatMatrixSource source,std::size_t start,std::size_t end,std::size_t features) noexcept {
    std::fill(sums_.begin(),sums_.end(),0.f);std::fill(counts_.begin(),counts_.end(),0);
    if(end<start)return false;
    const float step=end>start?float(config_.subdivisions)/float(end-start):0;
    auto row=std::span(row_).first(features);
    for(std::size_t frame=start;frame<end;++frame){
        if(!source.read(source.context,frame,row) ||
           !std::all_of(row.begin(),row.end(),[](float x){return std::isfinite(x);}))return false;
        const auto subdivision=std::min(config_.subdivisions-1,std::size_t(std::floor(float(frame-start)*step)));
        accumulate_row(row,subdivision);
    }
    return std::all_of(sums_.begin(),sums_.end(),[](float x){return std::isfinite(x);});
}
bool PreparedBeatFeatureSynchronization::write(FloatMatrixDestination output,std::size_t interval,std::size_t features) noexcept {
    auto row=std::span(row_).first(features);
    for(std::size_t subdivision=0;subdivision<config_.subdivisions;++subdivision){
        const auto count=counts_[subdivision];
        if(!count)++empty_count_;
        for(std::size_t feature=0;feature<features;++feature)
            row[feature]=count?sums_[subdivision*maximum_features_+feature]/float(count):
                (config_.empty==EmptySubdivision::Zero?0.f:std::numeric_limits<float>::quiet_NaN());
        if(!output.write(output.context,interval*config_.subdivisions+subdivision,row))return false;
    }
    return true;
}
bool PreparedBeatFeatureSynchronization::process(FloatMatrixSource source,std::size_t frames,std::size_t features,
                                                std::span<const float> beats,FloatMatrixDestination output) noexcept {
    retained_beats_=0;empty_count_=0;std::size_t start=0;
    if(!maximum_features_ || !features || features>maximum_features_ || !source.read || !output.write ||
       !boundaries(frames,beats,start) ||
       (retained_beats_ && retained_beats_-1>std::numeric_limits<std::size_t>::max()/config_.subdivisions))return false;
    for(std::size_t i=0;i+1<retained_beats_;++i){
        std::size_t end=0;
        if(!interval_end(beats,i,frames,end))return false;
        if(!interval(source,start,end,features) || !write(output,i,features))return false;
        start=end;
    }
    return true;
}
}


namespace dsp_kernels {
bool PreparedBeatFeatureSynchronization::storage_bytes(std::size_t features,BeatFeatureSynchronizationConfig c,std::size_t& out) noexcept {
    std::size_t bytes=0,count=0;
    if(!features || !c.subdivisions || !std::isfinite(c.frame_rate) || c.frame_rate<=0 ||
       unsigned(c.empty)>unsigned(EmptySubdivision::Zero) || !prepared_storage::mul_bytes(features,c.subdivisions,count) ||
       !prepared_storage::add_array(bytes,count,sizeof(float)) || !prepared_storage::add_array(bytes,features,sizeof(float)) ||
       !prepared_storage::add_array(bytes,c.subdivisions,sizeof(std::size_t)))return false;
    out=bytes;return true;
}
std::size_t PreparedBeatFeatureSynchronization::workspace_bytes() const noexcept {
    return (sums_.capacity()+row_.capacity())*sizeof(float)+counts_.capacity()*sizeof(std::size_t);
}
}
