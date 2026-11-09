// madmom features/onsets.py classical functions, pinned revision
// 27f032e8947204902c675e5e341a3faf5dc86dae.
// Copyright (c) 2012-2018 Department of Computational Perception, JKU, and OFAI.
// BSD-style notice/disclaimer: packaging/licenses/madmom.txt.
#include <dsp_kernels/spectral_novelty.h>
#include <algorithm>
#include <cmath>
#include <utility>
namespace dsp_kernels {
bool spectral_novelty_uses_phase(SpectralNoveltyMethod m) noexcept {
    return m==SpectralNoveltyMethod::PhaseMaskedDifference || m==SpectralNoveltyMethod::PhaseDeviation ||
        m==SpectralNoveltyMethod::WeightedPhaseDeviation || m==SpectralNoveltyMethod::NormalizedWeightedPhaseDeviation ||
        m==SpectralNoveltyMethod::ComplexDifference || m==SpectralNoveltyMethod::RectifiedComplexDifference;
}
bool spectral_novelty_uses_overlap_lag(SpectralNoveltyMethod m) noexcept {
    return m==SpectralNoveltyMethod::SquaredDifference || m==SpectralNoveltyMethod::PositiveDifference ||
        m==SpectralNoveltyMethod::MaximumDifference || m==SpectralNoveltyMethod::PhaseMaskedDifference ||
        m==SpectralNoveltyMethod::RectifiedComplexDifference;
}
namespace {
void complex_recipe(SpectralDescriptorConfig& d,SpectralNoveltyMethod method) noexcept {
    d.method=SpectralDescriptor::ComplexDifference;d.first_prediction_zero=true;
    d.half_wave=method==SpectralNoveltyMethod::RectifiedComplexDifference;d.strict_half_wave=true;
}
void phase_recipe(SpectralDescriptorConfig& d,SpectralNoveltyMethod method) noexcept {
    d.method=method==SpectralNoveltyMethod::NormalizedWeightedPhaseDeviation?
        SpectralDescriptor::NormalizedWeightedPhaseDeviation:SpectralDescriptor::PhaseDeviation;
    d.weighted=method!=SpectralNoveltyMethod::PhaseDeviation;d.warmup_frames=2;
    d.aggregation=SpectralAggregation::Mean;d.normalized_phase_mean=true;
}
SpectralDescriptorConfig descriptor_recipe(SpectralNoveltyConfig c) noexcept {
    SpectralDescriptorConfig d;d.compensated_sum=false;d.epsilon=c.epsilon;
    d.magnitude_gate=false;d.allow_signed_magnitude=true;d.stable_log1p=false;
    if(c.method==SpectralNoveltyMethod::HighFrequencyContent){
        d.method=SpectralDescriptor::HighFrequencyContent;d.bin_offset=0;d.aggregation=SpectralAggregation::Mean;
    }else if(c.method==SpectralNoveltyMethod::ModifiedKullbackLeibler){
        d.method=SpectralDescriptor::ModifiedKullbackLeibler;d.aggregation=SpectralAggregation::Mean;
    }else if(c.method==SpectralNoveltyMethod::ComplexDifference || c.method==SpectralNoveltyMethod::RectifiedComplexDifference)
        complex_recipe(d,c.method);
    else phase_recipe(d,c.method);
    return d;
}
}
bool PreparedSpectralNovelty::prepare(std::size_t frames,std::size_t features,std::size_t bins,
                                      SpectralNoveltyConfig config,std::span<const FilterBandSupport> supports) {
    std::size_t bound=0;
    if(!storage_bytes(frames,features,bins,config,supports.size(),bound))return false;
    PreparedSpectralNovelty next;
    if(!next.configure(frames,features,bins,config,supports) || next.workspace_bytes()>bound)return false;
    *this=std::move(next);return true;
}
bool PreparedSpectralNovelty::configure(std::size_t frames,std::size_t features,std::size_t bins,
                                        SpectralNoveltyConfig c,std::span<const FilterBandSupport> supports) {
    if(!frames || !features || !bins || !c.lag || unsigned(c.method)>unsigned(SpectralNoveltyMethod::AlignedDifference) ||
        !std::isfinite(c.epsilon) || c.epsilon<=0)return false;
    const bool shaped=spectral_novelty_uses_phase(c.method) && c.method!=SpectralNoveltyMethod::PhaseMaskedDifference &&
        c.method!=SpectralNoveltyMethod::PhaseDeviation;
    if(shaped && features!=bins)return false;
    config_=c;maximum_frames_=frames;features_=features;phase_bins_=bins;
    current_.resize(features);previous_.resize(features);differences_.resize(features);phase_.resize(bins);
    unit_magnitude_.assign(bins,1.f);ones_.assign(features,1.f);result_.resize(output_features());
    return prepare_operation(supports);
}
bool PreparedSpectralNovelty::prepare_phase_mask(std::span<const FilterBandSupport> supports) {
    ComplexFluxConfig mask;
    mask.lag=config_.lag;mask.frequency_maximum=config_.frequency_maximum;
    mask.temporal_maximum=config_.temporal_maximum;mask.temporal_origin=config_.temporal_origin;mask.compensated_sum=false;
    return phase_mask_.prepare(maximum_frames_,features_,phase_bins_,supports,mask);
}
bool PreparedSpectralNovelty::prepare_difference() {
    OnsetStrengthConfig difference;
    difference.center=false;difference.aggregate=OnsetAggregate::Sum;
    difference.max_size=config_.method==SpectralNoveltyMethod::MaximumDifference?std::max(std::size_t{1},config_.frequency_maximum):1;
    return difference_.prepare(features_,maximum_frames_,difference);
}
bool PreparedSpectralNovelty::relative_descriptor(FloatMatrixSource source,std::size_t frame,float& output) noexcept {
    if(frame<config_.lag){output=0;return true;}
    if(!read(source,frame,current_) || !read(source,frame-config_.lag,previous_))return false;
    return descriptor_.process_values(current_,output,previous_);
}
bool PreparedSpectralNovelty::prepare_operation(std::span<const FilterBandSupport> supports) {
    const auto method=config_.method;
    if(method==SpectralNoveltyMethod::PhaseMaskedDifference)return prepare_phase_mask(supports);
    if(method==SpectralNoveltyMethod::AlignedDifference)return alignment_.prepare(features_,config_.alignment_radius,config_.positive_alignment);
    if(method==SpectralNoveltyMethod::SquaredDifference || method==SpectralNoveltyMethod::PositiveDifference || method==SpectralNoveltyMethod::MaximumDifference)return prepare_difference();
    const auto count=method==SpectralNoveltyMethod::PhaseDeviation?phase_bins_:features_;
    return descriptor_.prepare(count,descriptor_recipe(config_));
}
void PreparedSpectralNovelty::reset() noexcept {next_frame_=0;descriptor_.reset();}
std::size_t PreparedSpectralNovelty::output_features() const noexcept {
    return config_.method==SpectralNoveltyMethod::AlignedDifference?features_:1;
}
bool PreparedSpectralNovelty::read(FloatMatrixSource source,std::size_t frame,std::span<float> values) noexcept {
    return source.read && source.read(source.context,frame,values) &&
        std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);});
}
bool PreparedSpectralNovelty::difference(FloatMatrixSource source,std::size_t frame,float& output) noexcept {
    if(!config_.supplied_differences && frame<config_.lag){output=0;return true;}
    if(!read(source,frame,current_))return false;
    if(config_.supplied_differences)std::copy(current_.begin(),current_.end(),differences_.begin());
    else if(!read(source,frame-config_.lag,previous_) || !difference_.positive_differences(current_,previous_,differences_))return false;
    if(config_.method==SpectralNoveltyMethod::SquaredDifference)for(auto& x:differences_)x*=x;
    return project_filter_weights(differences_,ones_,output,{false,false});
}
bool PreparedSpectralNovelty::phase_descriptor(FloatMatrixSource source,FloatMatrixSource phase,std::size_t frame,float& output) noexcept {
    if(!read(phase,frame,phase_))return false;
    if(config_.method==SpectralNoveltyMethod::PhaseDeviation)return descriptor_.process_polar(unit_magnitude_,phase_,output);
    if(!read(source,frame,current_))return false;
    if(config_.method!=SpectralNoveltyMethod::RectifiedComplexDifference)return descriptor_.process_polar(current_,phase_,output);
    if(frame<config_.lag)std::copy(current_.begin(),current_.end(),previous_.begin());
    else if(!read(source,frame-config_.lag,previous_))return false;
    return descriptor_.process_polar(current_,phase_,output,previous_);
}
bool PreparedSpectralNovelty::scalar(FloatMatrixSource source,FloatMatrixSource phase,std::size_t frames,std::size_t frame,float& output) noexcept {
    const auto method=config_.method;
    if(method==SpectralNoveltyMethod::PhaseMaskedDifference)return phase_mask_.process_frame(source,phase,frames,frame,output);
    if(spectral_novelty_uses_phase(method))return phase_descriptor(source,phase,frame,output);
    if(method==SpectralNoveltyMethod::SquaredDifference || method==SpectralNoveltyMethod::PositiveDifference || method==SpectralNoveltyMethod::MaximumDifference)
        return difference(source,frame,output);
    if(method==SpectralNoveltyMethod::ModifiedKullbackLeibler)return relative_descriptor(source,frame,output);
    return read(source,frame,current_) && descriptor_.process_values(current_,output);
}
bool PreparedSpectralNovelty::aligned(FloatMatrixSource source,std::size_t frame,std::span<float> output) noexcept {
    if(frame<config_.lag){std::fill(output.begin(),output.end(),0.f);return true;}
    return read(source,frame,current_) && read(source,frame-config_.lag,previous_) && alignment_.process(current_,previous_,output);
}
bool PreparedSpectralNovelty::process_frame(FloatMatrixSource source,FloatMatrixSource phase,std::size_t frames,
                                            std::size_t frame,std::span<float> output) noexcept {
    if(!maximum_frames_ || frames>maximum_frames_ || frame>=frames || frame!=next_frame_ || output.size()!=output_features())return false;
    const bool ok=config_.method==SpectralNoveltyMethod::AlignedDifference?aligned(source,frame,output):scalar(source,phase,frames,frame,output[0]);
    if(ok)++next_frame_;return ok;
}
bool PreparedSpectralNovelty::process(FloatMatrixSource source,FloatMatrixSource phase,std::size_t frames,FloatMatrixDestination output) noexcept {
    reset();
    if(!maximum_frames_ || frames>maximum_frames_ || !output.write)return false;
    for(std::size_t frame=0;frame<frames;++frame)
        if(!process_frame(source,phase,frames,frame,result_) || !output.write(output.context,frame,result_))return false;
    return true;
}
}
