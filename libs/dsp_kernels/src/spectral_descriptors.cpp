#include <dsp_kernels/spectral_descriptors.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <functional>
#include <numbers>
#include <utility>

namespace dsp_kernels {
namespace {
bool valid_weighting(const SpectralDescriptorConfig& c) noexcept {
    return std::isfinite(c.frequency_step) && c.frequency_step>=0 &&
        std::isfinite(c.frequency_exponent) && c.frequency_exponent>0;
}
bool valid_divergence(const SpectralDescriptorConfig& c) noexcept {
    return unsigned(c.divergence_arithmetic)<=unsigned(SpectralDivergenceArithmetic::Float64RatioFloat32Log) &&
        std::isfinite(c.ratio_epsilon) && c.ratio_epsilon>=0 &&
        (c.divergence_arithmetic==SpectralDivergenceArithmetic::Float32 || (!c.stable_log1p && c.ratio_epsilon>0));
}
bool valid(const SpectralDescriptorConfig& c) noexcept {
    return c.method>=SpectralDescriptor::Flux && c.method<=SpectralDescriptor::L2Flux &&
        std::isfinite(c.magnitude_threshold) && c.magnitude_threshold>=0 &&
        std::isfinite(c.epsilon) && c.epsilon>0 && std::isfinite(c.bin_offset) && c.bin_offset>=0 &&
        c.aggregation>=SpectralAggregation::MethodDefault && c.aggregation<=SpectralAggregation::Mean && valid_weighting(c) &&
        valid_divergence(c) && unsigned(c.complex_distance)<=unsigned(ComplexDistanceMode::SquaredCosineRaw);
}
struct Sum {
    float value=0,error=0;
    void add(float term,bool compensated=true) noexcept {
        if(!compensated){value+=term;return;}
        const float corrected=term-error,next=value+corrected;
        error=(next-value)-corrected;value=next;
    }
};
}
bool PreparedSpectralDescriptor::storage_bytes(std::size_t bins,std::size_t& bytes) noexcept {
    if(!bins || bins>std::numeric_limits<std::size_t>::max()/(5*sizeof(float)))return false;
    bytes=bins*5*sizeof(float);return true;
}
bool PreparedSpectralDescriptor::prepare(std::size_t bins,const SpectralDescriptorConfig& config) {
    std::size_t bytes=0;if(!valid(config) || !storage_bytes(bins,bytes))return false;
    if(config.mirror_spectrum && (config.source_fft_size<2 || bins!=config.source_fft_size/2+1))return false;
    PreparedSpectralDescriptor next;next.config_=config;
    next.magnitude_.resize(bins);next.phase_.resize(bins);next.previous_magnitude_.resize(bins);
    next.previous_phase_.resize(bins);next.older_phase_.resize(bins);
    *this=std::move(next);return true;
}
bool PreparedSpectralDescriptor::set_parameters(const SpectralDescriptorConfig& config) noexcept {
    if(magnitude_.empty() || !valid(config))return false;
    if(config.mirror_spectrum && (config.source_fft_size<2 || magnitude_.size()!=config.source_fft_size/2+1))return false;
    config_=config;return true;
}
void PreparedSpectralDescriptor::reset() noexcept {
    processed_frames_=0;
    std::fill(previous_magnitude_.begin(),previous_magnitude_.end(),0.f);
    std::fill(previous_phase_.begin(),previous_phase_.end(),0.f);
    std::fill(older_phase_.begin(),older_phase_.end(),0.f);
}
bool PreparedSpectralDescriptor::polar(std::span<const float> input) noexcept {
    if(magnitude_.empty() || input.size()!=2*magnitude_.size())return false;
    return cartesian_to_polar(input,magnitude_,phase_);
}
float PreparedSpectralDescriptor::phase_deviation(std::size_t i) const noexcept {
    return std::remainder(phase_[i]-2.f*previous_phase_[i]+older_phase_[i],2.f*std::numbers::pi_v<float>);
}
float PreparedSpectralDescriptor::divergence(std::size_t i,float reference) const noexcept {
    float logarithm=0;
    if(config_.divergence_arithmetic==SpectralDivergenceArithmetic::Float64RatioFloat32Log){
        const double argument=1.+double(magnitude_[i])/(double(reference)+config_.ratio_epsilon);
        if(!std::isfinite(argument) || std::abs(argument)>std::numeric_limits<float>::max())return std::numeric_limits<float>::quiet_NaN();
        logarithm=std::log(float(argument));
    }else{
        const float ratio=magnitude_[i]/(reference+config_.epsilon);
        logarithm=config_.stable_log1p?std::log1p(ratio):std::log(1.f+ratio);
    }
    return config_.method==SpectralDescriptor::KullbackLeibler?magnitude_[i]*logarithm:logarithm;
}
float PreparedSpectralDescriptor::phase_value(std::size_t i) const noexcept {
    if(config_.magnitude_gate && magnitude_[i]<=config_.magnitude_threshold)return 0;
    const bool weighted=config_.weighted || config_.method==SpectralDescriptor::NormalizedWeightedPhaseDeviation;
    return std::abs(phase_deviation(i)*(weighted?magnitude_[i]:1.f));
}
float PreparedSpectralDescriptor::multiplicity(std::size_t i) const noexcept {
    return config_.mirror_spectrum && i && config_.source_fft_size-i!=i?2.f:1.f;
}
float PreparedSpectralDescriptor::complex_difference(std::size_t i,float reference) const noexcept {
    const float magnitude=magnitude_[i],previous=previous_magnitude_[i];
    if(config_.half_wave && (magnitude<reference || (config_.strict_half_wave && magnitude==reference)))return 0;
    if(config_.complex_distance==ComplexDistanceMode::SquaredCosineRaw){
        // BTrack evaluates this law-of-cosines expression in double. Float32
        // products can round its nonnegative radicand below zero for adjacent
        // magnitudes, rejecting an otherwise finite source ODF frame.
        const double current=magnitude,old=previous;
        const double deviation=double(phase_[i])-2.*double(previous_phase_[i])+double(older_phase_[i]);
        return float(std::sqrt(current*current+old*old-2.*current*old*std::cos(deviation)));
    }
    if(config_.complex_distance==ComplexDistanceMode::SquaredCosine){
        const float predicted=config_.first_prediction_zero && processed_frames_==1?0:2.f*previous_phase_[i]-older_phase_[i];
        return std::sqrt(std::abs(previous*previous+magnitude*magnitude-2.f*previous*magnitude*std::cos(predicted-phase_[i])));
    }
    const auto deviation=config_.first_prediction_zero && processed_frames_==1?phase_[i]:phase_deviation(i);
    return std::hypot(previous-magnitude*std::cos(deviation),magnitude*std::sin(deviation));
}
float PreparedSpectralDescriptor::frequency_weight(std::size_t i) const noexcept {
    const float coordinate=(float(i)+config_.bin_offset)*config_.frequency_step;
    return config_.frequency_exponent==1?coordinate:std::pow(coordinate,config_.frequency_exponent);
}
float PreparedSpectralDescriptor::high_frequency(std::size_t i,float reference) const noexcept {
    float weight=frequency_weight(i);
    if(multiplicity(i)==2)weight=(weight+frequency_weight(config_.source_fft_size-i))*.5f;
    if(config_.method==SpectralDescriptor::HighFrequencyDifference){
        const float change=magnitude_[i]-reference;
        return weight*(config_.half_wave?std::max(0.f,change):std::abs(change));
    }
    return weight*(config_.squared?magnitude_[i]*magnitude_[i]:magnitude_[i]);
}
float PreparedSpectralDescriptor::contribution(std::size_t i,float reference) const noexcept {
    const float m=magnitude_[i],old=reference;
    switch(config_.method){
    case SpectralDescriptor::Flux:return config_.half_wave?std::max(0.f,m-old):std::abs(m-old);
    case SpectralDescriptor::ComplexDifference:return complex_difference(i,reference);
    case SpectralDescriptor::PhaseDeviation:
    case SpectralDescriptor::NormalizedWeightedPhaseDeviation:return phase_value(i);
    case SpectralDescriptor::HighFrequencyDifference:
    case SpectralDescriptor::HighFrequencyContent:return high_frequency(i,reference);
    case SpectralDescriptor::L2Flux:
    case SpectralDescriptor::SquaredFlux:{
        const float difference=config_.half_wave?std::max(0.f,m-old):m-old;return difference*difference;
    }
    case SpectralDescriptor::KullbackLeibler:
    case SpectralDescriptor::ModifiedKullbackLeibler:return divergence(i,reference);
    case SpectralDescriptor::SpectralEnergy:return m*m;
    }
    return 0;
}
void PreparedSpectralDescriptor::commit() noexcept {
    if(processed_frames_<SIZE_MAX)++processed_frames_;
    if(config_.update_phase_history){
        std::copy(previous_phase_.begin(),previous_phase_.end(),older_phase_.begin());
        std::copy(phase_.begin(),phase_.end(),previous_phase_.begin());
    }
    if(config_.update_magnitude_history)std::copy(magnitude_.begin(),magnitude_.end(),previous_magnitude_.begin());
}
float PreparedSpectralDescriptor::finish(float sum) const noexcept {
    if(processed_frames_<config_.warmup_frames)return 0;
    const float count=float(config_.mirror_spectrum?config_.source_fft_size:magnitude_.size());
    if(config_.method==SpectralDescriptor::NormalizedWeightedPhaseDeviation){
        Sum level;for(std::size_t i=0;i<magnitude_.size();++i)level.add(magnitude_[i]*multiplicity(i),config_.compensated_sum);
        return config_.normalized_phase_mean?(sum/count)/(level.value/count+config_.epsilon):sum/(level.value+count*config_.epsilon);
    }
    const bool mean=config_.aggregation==SpectralAggregation::Mean ||
        (config_.aggregation==SpectralAggregation::MethodDefault && config_.method==SpectralDescriptor::PhaseDeviation);
    const float value=mean?sum/count:sum;
    return config_.method==SpectralDescriptor::L2Flux?std::sqrt(value):value;
}
bool PreparedSpectralDescriptor::evaluate(float& output,std::span<const float> reference) noexcept {
    if(reference.empty())reference=previous_magnitude_;
    Sum sum;
    for(std::size_t i=0;i<magnitude_.size();++i)sum.add(contribution(i,reference[i])*multiplicity(i),config_.compensated_sum);
    const float result=finish(sum.value);
    if(!std::isfinite(result))return false;
    commit();output=result;return true;
}
bool PreparedSpectralDescriptor::process(std::span<const float> input,float& output) noexcept {
    return polar(input) && evaluate(output);
}
bool PreparedSpectralDescriptor::process_polar(std::span<const float> magnitude,std::span<const float> phase,float& output,
                                               std::span<const float> reference) noexcept {
    if(magnitude_.empty() || magnitude.size()!=magnitude_.size() || phase.size()!=magnitude.size() ||
        (!reference.empty() && reference.size()!=magnitude.size()))return false;
    for(std::size_t i=0;i<magnitude.size();++i)
        if(!std::isfinite(magnitude[i]) || (!config_.allow_signed_magnitude && magnitude[i]<0) || !std::isfinite(phase[i]))return false;
    if(!std::all_of(reference.begin(),reference.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(magnitude.begin(),magnitude.end(),magnitude_.begin());std::copy(phase.begin(),phase.end(),phase_.begin());
    return evaluate(output,reference);
}
bool PreparedSpectralDescriptor::values_only() const noexcept {
    return config_.method!=SpectralDescriptor::ComplexDifference &&
        config_.method!=SpectralDescriptor::PhaseDeviation && config_.method!=SpectralDescriptor::NormalizedWeightedPhaseDeviation;
}
bool PreparedSpectralDescriptor::process_values(std::span<const float> values,float& output,std::span<const float> reference) noexcept {
    if(!values_only() || magnitude_.empty() || values.size()!=magnitude_.size() || (!reference.empty() && reference.size()!=values.size()))return false;
    if(!std::all_of(values.begin(),values.end(),[](float x){return std::isfinite(x);}) ||
       !std::all_of(reference.begin(),reference.end(),[](float x){return std::isfinite(x);}))return false;
    std::copy(values.begin(),values.end(),magnitude_.begin());std::fill(phase_.begin(),phase_.end(),0.f);
    return evaluate(output,reference);
}
bool PreparedSpectralDescriptor::copy_state_from(const PreparedSpectralDescriptor& other) noexcept {
    if(config_!=other.config_ || magnitude_.size()!=other.magnitude_.size())return false;
    processed_frames_=other.processed_frames_;
    std::copy(other.previous_magnitude_.begin(),other.previous_magnitude_.end(),previous_magnitude_.begin());
    std::copy(other.previous_phase_.begin(),other.previous_phase_.end(),previous_phase_.begin());
    std::copy(other.older_phase_.begin(),other.older_phase_.end(),older_phase_.begin());return true;
}
}
