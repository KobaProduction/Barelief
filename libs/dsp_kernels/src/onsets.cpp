#include <dsp_kernels/onsets.h>
#include <dsp_kernels/onset_strength.h>
#include <dsp_kernels/sequence_statistics.h>
#include <dsp_kernels/order_statistics.h>
#include <algorithm>
#include <cmath>

namespace dsp_kernels {
namespace {
bool valid_config(const OnsetConfig& c) noexcept {
    if(!c.feature_count || c.feature_count>max_onset_features || c.channel_count>max_onset_channels)return false;
    if(!std::isfinite(c.contrast_floor) || c.contrast_floor<=0 || c.contrast_floor>=1)return false;
    if(!std::isfinite(c.noise_multiplier) || c.noise_multiplier<0)return false;
    if(!std::isfinite(c.min_positive_mass) || c.min_positive_mass<=0)return false;
    if(!std::isfinite(c.refractory_s) || c.refractory_s<0)return false;
    return std::all_of(c.channels.begin(),c.channels.begin()+c.channel_count,[&](OnsetBand b){
        return b.count && b.first<c.feature_count && b.count<=c.feature_count-b.first;
    });
}
float median(std::span<const float> input) noexcept {
    if(input.empty())return 0;
    std::array<float,onset_noise_frames> sorted{};
    std::copy(input.begin(),input.end(),sorted.begin());
    return median_in_place(std::span{sorted}.first(input.size()),EvenMedian::Mean);
}
float adaptive_threshold(std::span<const float> history,const OnsetConfig& config) noexcept {
    const float center=median(history);
    std::array<float,onset_noise_frames> deviations{};
    std::transform(history.begin(),history.end(),deviations.begin(),[&](float value){return std::abs(value-center);});
    const float spread=median(std::span{deviations}.first(history.size()));
    return std::max(config.contrast_floor,center+config.noise_multiplier*spread);
}
}
bool PreparedOnsets::prepare(const OnsetConfig& config) noexcept {
    if(!valid_config(config))return false;
    config_=config;channel_count_=std::max(1U,config.channel_count);reset();return true;
}
void PreparedOnsets::reset() noexcept {
    previous_.fill(0);current_.fill(0);differences_.fill(0);channels_.fill({});output_.fill({});last_time_.reset();
}
std::size_t PreparedOnsets::Input::size() const noexcept {return floats.empty()?bytes.size():floats.size();}
float PreparedOnsets::Input::value(std::size_t index) const noexcept {
    return floats.empty()?float(bytes[index])/255.f:floats[index];
}
bool PreparedOnsets::valid_input(const Input& input,double observed,double ready) const noexcept {
    if(!channel_count_ || input.size()!=config_.feature_count)return false;
    if(!std::isfinite(observed) || !std::isfinite(ready) || observed<0 || ready<observed)return false;
    if(last_time_ && observed<=*last_time_)return false;
    return std::all_of(input.floats.begin(),input.floats.end(),[](float value){return std::isfinite(value) && value>=0;});
}
OnsetBand PreparedOnsets::band(unsigned channel) const noexcept {
    return config_.channel_count?config_.channels[channel]:OnsetBand{0,config_.feature_count};
}
bool PreparedOnsets::measure(OnsetBand range,Measurement& output) const noexcept {
    const auto current=std::span(current_).subspan(range.first,range.count);
    float positive=0,level=0,previous=0;
    if(!sum_float32(std::span(differences_).subspan(range.first,range.count),positive) ||
       !sum_float32(current,level) || !sum_float32(std::span(previous_).subspan(range.first,range.count),previous))return false;
    const auto clipped=unsigned(std::count_if(current.begin(),current.end(),[](float value){return value>1;}));
    output.observation={positive/float(range.count),positive/std::max({1.f/255.f,level,previous}),0,level/float(range.count),clipped,{}};
    output.positive_mass=positive;
    return true;
}
bool PreparedOnsets::measure_channel(unsigned channel,Measurement& measured) const noexcept {
    if(!measure(band(channel),measured))return false;
    auto& value=measured.observation;
    value.threshold=adaptive_threshold(std::span{channels_[channel].noise}.first(channels_[channel].noise_count),config_);
    return std::isfinite(value.threshold);
}
void PreparedOnsets::publish_measurements(std::span<const Measurement> measured,double observed,double ready) noexcept {
    for(unsigned i=0;i<channel_count_;++i){
        output_[i]=measured[i].observation;
        if(!last_time_){output_[i].flux=0;output_[i].contrast=0;}
        detect(i,output_[i],measured[i].positive_mass,observed,ready);
    }
}
bool PreparedOnsets::measure_input(const Input& input,std::span<Measurement> measured) noexcept {
    auto current=std::span(current_).first(input.size());
    for(std::size_t i=0;i<input.size();++i)current[i]=input.value(i);
    if(!spectral_difference_values(current,std::span(previous_).first(input.size()),std::span(differences_).first(input.size())))return false;
    for(unsigned i=0;i<channel_count_;++i)if(!measure_channel(i,measured[i]))return false;
    return true;
}
void PreparedOnsets::detect(unsigned channel,OnsetObservation& value,float positive_mass,double observed,double ready) noexcept {
    auto& state=channels_[channel];
    if(state.pending && value.contrast<state.previous_contrast){
        if(!state.last_event || state.pending->t_obs-*state.last_event>=config_.refractory_s){
            value.peak=state.pending;value.peak->t_ready=ready;state.last_event=value.peak->t_obs;
        }
        state.pending.reset();
    }
    if(value.contrast>state.previous_contrast && value.contrast>=value.threshold &&
       positive_mass>=config_.min_positive_mass)
        state.pending=OnsetPeak{observed,ready,value.contrast,positive_mass};
    state.previous_contrast=value.contrast;
    state.noise[state.noise_write]=value.contrast;state.noise_write=(state.noise_write+1)%onset_noise_frames;
    state.noise_count=std::min(state.noise_count+1,unsigned(onset_noise_frames));
    value.pending_t_obs=state.pending?std::optional{state.pending->t_obs}:std::nullopt;
}
bool PreparedOnsets::process_input(const Input& input,double observed,double ready) noexcept {
    if(!valid_input(input,observed,ready))return false;
    std::array<Measurement,max_onset_channels> measured{};
    if(!measure_input(input,std::span(measured).first(channel_count_)))return false;
    publish_measurements(std::span(measured).first(channel_count_),observed,ready);
    for(unsigned i=0;i<config_.feature_count;++i)previous_[i]=input.value(i);
    last_time_=observed;return true;
}
bool PreparedOnsets::process(std::span<const float> values,double observed,double ready) noexcept {
    return process_input({values,{}},observed,ready);
}
bool PreparedOnsets::process(std::span<const std::uint8_t> values,double observed,double ready) noexcept {
    return process_input({{},values},observed,ready);
}
std::span<const OnsetObservation> PreparedOnsets::output() const noexcept {return std::span{output_}.first(channel_count_);}
std::size_t PreparedOnsets::workspace_bytes() noexcept {return sizeof(PreparedOnsets);}
}
