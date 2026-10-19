#include <dsp_kernels/prepared_resampler.h>
#include <numbers>

namespace dsp_kernels {
namespace {
bool fir_mode(ResamplerMode mode) noexcept {
    return mode==ResamplerMode::ExistingFir || mode==ResamplerMode::CompactFir;
}
PcmResampler::Quality fir_quality(ResamplerMode mode) noexcept {
    return mode==ResamplerMode::CompactFir?PcmResampler::Quality::Compact:PcmResampler::Quality::Full;
}
bool valid_config(const ResamplerConfig& config) noexcept {
    return PreparedResampler::supported(config.input_rate,config.output_rate)&&config.channels&&config.channels<=2&&config.quality<=10&&
        (config.mode!=ResamplerMode::Bypass||config.input_rate==config.output_rate);
}
ResamplerResources fir_resources(const PcmResampler::Geometry& geometry) noexcept {
    ResamplerResources result;result.coefficient_bytes=geometry.coefficient_bytes;result.state_bytes=geometry.state_bytes;
    result.fixed_delay_input_samples=geometry.delay;return result;
}
ResamplerResources mode_resources(const ResamplerConfig& config) noexcept {
    ResamplerResources result;result.experimental=config.mode==ResamplerMode::IirExperimental;
    result.frequency_dependent_delay=result.experimental&&config.input_rate!=config.output_rate;
    result.alias_warning=config.mode==ResamplerMode::LinearDiagnostic&&config.output_rate<config.input_rate;return result;
}
bool finite_channels(std::span<const float> values,std::size_t count,std::size_t stride,std::size_t channels) noexcept {
    for(std::size_t channel=0;channel<channels;++channel)for(std::size_t i=0;i<count;++i)if(!std::isfinite(values[channel*stride+i]))return false;
    return true;
}

}
PreparedResampler::PreparedResampler(const PreparedResampler& other) {
    if(other.prepared_ && prepare(other.config_,other.factory_))copy_state_from(other);
}
PreparedResampler& PreparedResampler::operator=(const PreparedResampler& other) {
    if(this!=&other){PreparedResampler candidate(other);*this=std::move(candidate);}return *this;
}
void PreparedResampler::design_iir() noexcept {
    const double frequency=.40*std::min(config_.input_rate,config_.output_rate)/config_.input_rate;
    const double omega=2.*std::numbers::pi*frequency,c=std::cos(omega),s=std::sin(omega);
    for(std::size_t section=0;section<3;++section){
        const double q=1./(2.*std::cos((2.*section+1.)*std::numbers::pi/12.));
        const double alpha=s/(2.*q),a0=1.+alpha;
        iir_[section]={float((1.-c)/(2.*a0)),float((1.-c)/a0),float((1.-c)/(2.*a0)),float(-2.*c/a0),float((1.-alpha)/a0)};
    }
}
bool PreparedResampler::prepare_mode(const ResamplerBackendFactory* external){
    switch(config_.mode){
    case ResamplerMode::Bypass:case ResamplerMode::LinearDiagnostic:return true;
    case ResamplerMode::ExistingFir:case ResamplerMode::CompactFir:return existing_.prepare(config_.input_rate,config_.output_rate,config_.channels,fir_quality(config_.mode));
    case ResamplerMode::IirExperimental:design_iir();return true;
    case ResamplerMode::SpeexDsp:if(!external)return false;backend_=external->prepare(config_);return bool(backend_);
    default:return false;
    }
}
bool PreparedResampler::prepare(const ResamplerConfig& cfg,const ResamplerBackendFactory* external) {
    if(!valid_config(cfg))return false;
    PreparedResampler next;next.config_=cfg;next.factory_=external;
    next.integer_step_=cfg.input_rate/cfg.output_rate;next.fractional_step_=cfg.input_rate%cfg.output_rate;
    next.inverse_output_=1.f/float(cfg.output_rate);
    if(!next.prepare_mode(external))return false;
    next.prepared_=true;next.reset();*this=std::move(next);return true;
}
void PreparedResampler::reset() noexcept {
    existing_.reset();if(backend_)backend_->reset();iir_state_={};previous_={};
    until_=0;fraction_=0;produced_=0;anchor_=next_input_=0;anchored_=input_expected_=has_input_=false;
}
bool PreparedResampler::start_at(std::int64_t output_tick,std::uint32_t numerator) noexcept {
    if(!prepared_ || has_input_ || anchored_ || numerator>=config_.input_rate)return false;
    if(fir_mode(config_.mode) && !existing_.initial_position(numerator))return false;
    if(backend_ && !backend_->initial_position(numerator,config_.output_rate))return false;
    until_=numerator/config_.output_rate+std::size_t(numerator%config_.output_rate!=0);
    fraction_=numerator%config_.output_rate;anchor_=output_tick;anchored_=true;return true;
}
bool PreparedResampler::copy_state_from(const PreparedResampler& other) noexcept {
    if(!prepared_ || !other.prepared_ || config_!=other.config_ || bool(backend_)!=bool(other.backend_))return false;
    if(fir_mode(config_.mode) && !existing_.copy_state_from(other.existing_))return false;
    if(backend_ && !backend_->copy_state_from(*other.backend_))return false;
    iir_state_=other.iir_state_;previous_=other.previous_;until_=other.until_;fraction_=other.fraction_;
    produced_=other.produced_;anchor_=other.anchor_;next_input_=other.next_input_;anchored_=other.anchored_;
    input_expected_=other.input_expected_;has_input_=other.has_input_;return true;
}
bool PreparedResampler::advance_input(std::int64_t ticks,std::size_t count) noexcept {
    if(!std::in_range<std::int64_t>(count) || ticks>INT64_MAX-std::int64_t(count))return false;
    next_input_=ticks+std::int64_t(count);input_expected_=true;return true;
}
ResamplerResources PreparedResampler::resources() const noexcept {
    ResamplerResources result;result.owned_bytes=sizeof(*this);if(!prepared_)return result;
    if(backend_){result=backend_->resources();result.owned_bytes+=sizeof(*this);return result;}
    result=fir_mode(config_.mode)?fir_resources(existing_.filter_geometry()):mode_resources(config_);
    result.owned_bytes=sizeof(*this)+result.coefficient_bytes+result.state_bytes;return result;
}
bool PreparedResampler::resource_requirements(const ResamplerConfig& config,const ResamplerBackendFactory* factory,ResamplerResources& result) noexcept {
    result={};if(!valid_config(config))return false;
    if(config.mode==ResamplerMode::SpeexDsp){if(!factory||!factory->query(config,result))return false;result.owned_bytes+=sizeof(PreparedResampler);return true;}
    switch(config.mode){
    case ResamplerMode::Bypass:case ResamplerMode::IirExperimental:case ResamplerMode::LinearDiagnostic:result=mode_resources(config);break;
    case ResamplerMode::ExistingFir:case ResamplerMode::CompactFir:result=fir_resources(PcmResampler::geometry(config.input_rate,config.output_rate,config.channels,fir_quality(config.mode)));break;
    default:return false;
    }
    result.owned_bytes=sizeof(PreparedResampler)+result.coefficient_bytes+result.state_bytes;return true;
}
bool PreparedResampler::reject_output(std::size_t& written) noexcept {reset();written=0;return false;}
bool PreparedResampler::process_fir(ProcessBuffers& buffers) noexcept {
    bool state_reset=false;
    if(!existing_.process(buffers.input,buffers.count,buffers.stride,buffers.output,buffers.capacity,buffers.written,&state_reset)){
        if(state_reset)reset();return false;
    }
    has_input_=has_input_||buffers.count!=0;produced_+=buffers.written;return true;
}
bool PreparedResampler::process_backend(ProcessBuffers& buffers) noexcept {
    if(!backend_->process(buffers.input,buffers.count,buffers.stride,buffers.output,buffers.capacity,buffers.written)||buffers.written>buffers.capacity)return reject_output(buffers.written);
    if(!finite_channels(buffers.output,buffers.written,buffers.capacity,config_.channels))return reject_output(buffers.written);
    produced_+=buffers.written;return true;
}
bool PreparedResampler::process_identity(ProcessBuffers& buffers) noexcept {
    for(std::size_t channel=0;channel<config_.channels;++channel)std::memcpy(buffers.output.data()+channel*buffers.capacity,buffers.input.data()+channel*buffers.stride,buffers.count*sizeof(float));
    buffers.written=buffers.count;produced_+=buffers.count;return true;
}
float PreparedResampler::filter_iir(float value,std::size_t channel) noexcept {
    for(std::size_t section=0;section<3;++section){
        const auto& q=iir_[section];auto& state=iir_state_[channel];const float output=q.b0*value+state[section*2];
        state[section*2]=q.b1*value-q.a1*output+state[section*2+1];state[section*2+1]=q.b2*value-q.a2*output;value=output;
    }
    return value;
}
bool PreparedResampler::filter_sample(const ProcessBuffers& buffers,std::size_t index,std::array<float,2>& current) noexcept {
    for(std::size_t channel=0;channel<config_.channels;++channel){
        float value=buffers.input[channel*buffers.stride+index];
        if(config_.mode==ResamplerMode::IirExperimental)value=filter_iir(value,channel);
        if(!std::isfinite(value))return false;current[channel]=value;
    }
    return true;
}
void PreparedResampler::advance_phase() noexcept {
    const auto old=fraction_;auto next=fraction_+fractional_step_;
    const auto carry=std::uint32_t(next>=config_.output_rate);if(carry)next-=config_.output_rate;
    until_=std::size_t(integer_step_+carry)+(next!=0)-(old!=0);fraction_=next;
}
bool PreparedResampler::emit_sample(ProcessBuffers& buffers,const std::array<float,2>& current) noexcept {
    const auto backwards=fraction_?config_.output_rate-fraction_:0u;const float back=float(backwards)*inverse_output_;
    for(std::size_t channel=0;channel<config_.channels;++channel){
        const float value=std::lerp(current[channel],previous_[channel],back);
        if(!std::isfinite(value))return false;buffers.output[channel*buffers.capacity+buffers.written]=value;
    }
    ++buffers.written;++produced_;advance_phase();return true;
}
bool PreparedResampler::process_interpolated(ProcessBuffers& buffers) noexcept {
    for(std::size_t index=0;index<buffers.count;++index){std::array<float,2> current{};
        if(!filter_sample(buffers,index,current))return reject_output(buffers.written);
        while(until_==0)if(!emit_sample(buffers,current))return reject_output(buffers.written);
        previous_=current;--until_;
    }
    return true;
}
bool PreparedResampler::process(std::span<const float> input,std::size_t count,std::size_t stride,
    std::span<float> output,std::size_t capacity,std::size_t& written) noexcept {
    written=0;const auto& config=config_;const auto bound=maximum_output(count,config.input_rate,config.output_rate);
    if(!prepared_||stride<count||stride>input.size()/config.channels||capacity>output.size()/config.channels||capacity<bound||produced_>UINT64_MAX-bound)return false;
    ProcessBuffers buffers{input,count,stride,output,capacity,written};
    if(fir_mode(config.mode))return process_fir(buffers);
    if(!finite_channels(input,count,stride,config.channels))return false;
    if(!count)return true;has_input_=true;
    if(backend_)return process_backend(buffers);
    if(config.input_rate==config.output_rate)return process_identity(buffers);
    return process_interpolated(buffers);
}
}
