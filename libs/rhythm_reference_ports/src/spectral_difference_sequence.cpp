// SpectrogramDifferenceProcessor and BufferProcessor semantics adapted from
// CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
// Uses existing isolated GPL scalar storage; D does not depend on R.
#include <rhythm_reference_ports/spectral_difference_sequence.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <utility>
#include <dsp_kernels/prepared_storage.h>
namespace rhythm_reference_ports {
bool PreparedSpectralDifferenceSequence::prepare(std::size_t frames,std::size_t features,SpectralDifferenceSequenceConfig config) {
    PreparedSpectralDifferenceSequence next;std::size_t bytes=0;
    if(!storage_bytes(frames,features,config,bytes) || !next.configure(frames,features,config) || next.workspace_bytes()>bytes)return false;
    *this=std::move(next);return true;
}
bool PreparedSpectralDifferenceSequence::storage_bytes(std::size_t frames,std::size_t features,SpectralDifferenceSequenceConfig config,std::size_t& output) noexcept {
    if(!frames || !features || !config.lag || config.lag>SIZE_MAX-frames || features>SIZE_MAX/(config.lag+frames))return false;
    dsp_kernels::OnsetStrengthConfig difference;difference.center=false;difference.max_size=std::max(std::size_t{1},config.frequency_maximum);
    std::size_t bytes=0,nested=0;
    if(!ScalarHistory::storage_bytes((config.lag+frames)*features,bytes) ||
       !dsp_kernels::PreparedOnsetStrength::storage_bytes(features,frames,difference,nested) ||
       !dsp_kernels::prepared_storage::add_bytes(bytes,nested) ||
       !dsp_kernels::prepared_storage::add_array(bytes,features,2*sizeof(float)))return false;
    output=bytes;return true;
}
bool PreparedSpectralDifferenceSequence::configure(std::size_t frames,std::size_t features,SpectralDifferenceSequenceConfig c) {
    if(!frames || !features || !c.lag || c.lag>SIZE_MAX-frames || features>SIZE_MAX/(c.lag+frames))return false;
    dsp_kernels::OnsetStrengthConfig difference;difference.center=false;difference.max_size=std::max(std::size_t{1},c.frequency_maximum);
    if(!history_.prepare((c.lag+frames)*features) || !difference_.prepare(features,frames,difference))return false;
    config_=c;maximum_chunk_=frames;features_=features;row_.resize(features);differences_.resize(features);return true;
}
void PreparedSpectralDifferenceSequence::reset() noexcept {started_=false;buffer_frames_=0;output_frames_=0;}
bool PreparedSpectralDifferenceSequence::initialize(std::size_t frames) noexcept {
    buffer_frames_=config_.lag+frames;
    if(!history_.reset_length(buffer_frames_*features_,std::numeric_limits<float>::infinity()))return false;
    started_=true;return true;
}
bool PreparedSpectralDifferenceSequence::append(dsp_kernels::FloatMatrixSource input,std::size_t frames) noexcept {
    for(std::size_t frame=0;frame<frames;++frame){
        if(!input.read(input.context,frame,row_) || !std::all_of(row_.begin(),row_.end(),[](float x){return std::isfinite(x);}))return false;
        for(float value:row_)history_.push(value);
    }
    return true;
}
bool PreparedSpectralDifferenceSequence::difference(std::span<const float> current,std::span<const float> reference) noexcept {
    if(std::any_of(reference.begin(),reference.end(),[](float x){return std::isinf(x);})){
        std::fill(differences_.begin(),differences_.end(),0.f);return true;
    }
    return difference_.differences(current,reference,differences_,{true,config_.positive,true});
}
bool PreparedSpectralDifferenceSequence::emit(dsp_kernels::FloatMatrixDestination output,dsp_kernels::FloatMatrixDestination current_output) noexcept {
    const auto history=history_.values();const auto count=buffer_frames_-config_.lag;
    for(std::size_t frame=0;frame<count;++frame){
        const auto current=history.subspan((frame+config_.lag)*features_,features_);
        if(!difference(current,history.subspan(frame*features_,features_)))return false;
        if(output.write && !output.write(output.context,frame,differences_))return false;
        if(current_output.write && !current_output.write(current_output.context,frame,current))return false;
    }
    output_frames_=count;return true;
}
bool PreparedSpectralDifferenceSequence::process(dsp_kernels::FloatMatrixSource input,std::size_t frames,
    dsp_kernels::FloatMatrixDestination output,bool reset_state,dsp_kernels::FloatMatrixDestination current) noexcept {
    output_frames_=0;
    if(!maximum_chunk_ || frames>maximum_chunk_ || (!output.write && !current.write) || (frames && !input.read))return false;
    if(reset_state)reset();
    if(!frames)return true;
    if(!started_ && !initialize(frames))return false;
    return append(input,frames) && emit(output,current);
}
}
