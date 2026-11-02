// SPDX-License-Identifier: ISC
// librosa feature/rhythm.py metrogram / core/harmonic.py interp_harmonics,
// pinned 3e4ff7bf5898ac5b326e7dee56463b4b325b91cb.
// Copyright (c) 2013--2026 librosa development team.
// Full notice: packaging/licenses/librosa.txt.
#include <dsp_kernels/prepared_storage.h>
#include <dsp_kernels/metrical_tempogram_sequence.h>
#include <array>
#include <algorithm>
#include <cstdint>
#include <utility>
namespace dsp_kernels {
bool PreparedMetricalTempogramSequence::prepare(std::span<const float> grid,MetricalTempogramSequenceConfig c,
    std::optional<std::span<const float>> factors) {
    std::size_t required=0;if(!storage_bytes(grid.size(),c,factors?factors->size():4,required))return false;
    PreparedMetricalTempogramSequence next;
    if(!next.configure(grid,c,factors))return false;
    if(next.workspace_bytes()>required)return false;
    *this=std::move(next);return true;
}
bool PreparedMetricalTempogramSequence::configure(std::span<const float> grid,MetricalTempogramSequenceConfig c,
    std::optional<std::span<const float>> supplied) {
    if(grid.empty() || unsigned(c.tempo_reduction.operation)>unsigned(FeatureReduction::Sum))return false;
    constexpr std::array defaults{1.f/3.f,.25f,.2f,1.f/7.f};
    const auto factors=supplied.value_or(std::span<const float>(defaults));
    if(factors.size()>SIZE_MAX/grid.size())return false;
    coordinates_.assign(grid.begin(),grid.end());factors_.assign(factors.begin(),factors.end());
    c.interpolation.keep_infinite_coordinates=true;c.interpolation.source_linear=true;config_=c;
    const bool curve=c.dynamic_frequencies?interpolation_.prepare_dynamic(grid.size(),c.interpolation):interpolation_.prepare(grid,c.interpolation);
    if(!curve)return false;
    input_.resize(grid.size());products_.resize(grid.size()*factors.size());
    output_.resize(!c.tempo_reduction.custom && c.tempo_reduction.operation==FeatureReduction::None?products_.size():factors.size());
    prepared_=true;return true;
}
bool PreparedMetricalTempogramSequence::read_frequency(const void* context,std::size_t bin,std::span<float> output) noexcept {
    const auto& owner=*static_cast<const PreparedMetricalTempogramSequence*>(context);
    if(bin>=owner.coordinates_.size() || output.size()!=owner.factors_.size())return false;
    for(std::size_t f=0;f<output.size();++f)output[f]=owner.products_[f*owner.coordinates_.size()+bin];
    return true;
}
bool PreparedMetricalTempogramSequence::reduce(FloatMatrixReductionPort custom) noexcept {
    const auto& c=config_.tempo_reduction;const auto bins=coordinates_.size();
    if(c.custom)return custom.reduce && custom.reduce(custom.context,{this,read_frequency},bins,factors_.size(),output_);
    if(c.operation==FeatureReduction::None){std::copy(products_.begin(),products_.end(),output_.begin());return true;}
    for(std::size_t factor=0;factor<factors_.size();++factor)
        if(!reduce_feature_values(std::span(products_).subspan(factor*bins,bins),c.operation,output_[factor],true))return false;
    return true;
}
bool PreparedMetricalTempogramSequence::evaluate(FloatMatrixSource source,FloatMatrixSource frequencies,
    std::size_t time,FloatMatrixReductionPort custom) noexcept {
    if(!source.read(source.context,time,input_))return false;
    if(config_.dynamic_frequencies){
        if(!frequencies.read(frequencies.context,time,coordinates_) || !interpolation_.set_coordinates(coordinates_))return false;
    }
    return interpolation_.set_values(input_) &&
        metrical_tempogram(interpolation_,coordinates_,input_,factors_,products_) && reduce(custom);
}
bool PreparedMetricalTempogramSequence::process_channels(std::span<const MetricalTempogramChannelInput> channels,FloatMatrixBatchDestination output) noexcept {
    if(!output.write)return false;
    for(std::size_t channel=0;channel<channels.size();++channel){
        const auto& input=channels[channel];FloatMatrixBatchChannel sink{output,channel};
        if(!process(input.values,input.frames,sink.destination(),input.frequencies,input.reduction))return false;
    }
    return true;
}
bool PreparedMetricalTempogramSequence::process(FloatMatrixSource source,std::size_t frames,FloatMatrixDestination output,
    FloatMatrixSource frequencies,FloatMatrixReductionPort custom) noexcept {
    if(!prepared_ || !source.read || !output.write || (config_.dynamic_frequencies && !frequencies.read) ||
        bool(custom.reduce)!=config_.tempo_reduction.custom)return false;
    for(std::size_t time=0;time<frames;++time)
        if(!evaluate(source,frequencies,time,custom) || !output.write(output.context,time,output_))return false;
    return true;
}
}


namespace dsp_kernels {
bool PreparedMetricalTempogramSequence::storage_bytes(std::size_t bins,MetricalTempogramSequenceConfig c,std::size_t factors,std::size_t& out) noexcept {
    if(!bins || unsigned(c.tempo_reduction.operation)>unsigned(FeatureReduction::Sum))return false;
    c.interpolation.keep_infinite_coordinates=true;c.interpolation.source_linear=true;
    std::size_t bytes=0,products=0;
    if(!prepared_storage::mul_bytes(bins,factors,products) || !PreparedCurveInterpolation::storage_bytes(bins,c.interpolation,bytes) ||
       !prepared_storage::add_array(bytes,bins,2*sizeof(float)) || !prepared_storage::add_array(bytes,factors,sizeof(float)) ||
       !prepared_storage::add_array(bytes,products,sizeof(float)) ||
       !prepared_storage::add_array(bytes,!c.tempo_reduction.custom && c.tempo_reduction.operation==FeatureReduction::None?products:factors,sizeof(float)))return false;
    if(c.dynamic_frequencies && !prepared_storage::add_array(bytes,bins,sizeof(float)))return false;
    out=bytes;return true;
}
std::size_t PreparedMetricalTempogramSequence::workspace_bytes() const noexcept {
    return interpolation_.workspace_bytes()+(coordinates_.capacity()+factors_.capacity()+input_.capacity()+products_.capacity()+output_.capacity())*sizeof(float);
}
}
