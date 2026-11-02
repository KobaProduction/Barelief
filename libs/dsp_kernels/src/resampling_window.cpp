#include <dsp_kernels/prepared_resampler.h>
#include <algorithm>
#include <cmath>
#include <functional>
namespace dsp_kernels {
namespace {
bool finite_window_channels(std::span<const float> values,std::size_t count,std::size_t stride,std::size_t channels) noexcept {
    for(std::size_t channel=0;channel<channels;++channel)
        for(float value:values.subspan(channel*stride,count))if(!std::isfinite(value))return false;
    return true;
}
bool compatible_buffers(std::span<const float> input,std::span<float> output,bool identity) noexcept {
    if(input.empty() || output.empty() || (identity && input.data()==output.data()))return true;
    const std::less<const float*> before;
    return !before(input.data(),output.data()+output.size()) || !before(output.data(),input.data()+input.size());
}
bool expand_segment(float first,float last,std::size_t factor,std::span<float> output) noexcept {
    const float delta=(last-first)/float(factor);
    if(!std::isfinite(delta))return false;
    for(std::size_t j=0;j<factor;++j){
        const float value=first+delta*float(j);
        if(!std::isfinite(value))return false;
        output[j]=value;
    }
    return true;
}
bool expand_channel(std::span<const float> input,std::size_t factor,std::span<float> output) noexcept {
    if(input.empty())return true;
    if(factor==1){
        if(input.data()!=output.data())std::copy(input.begin(),input.end(),output.begin());
        return true;
    }
    for(std::size_t i=0;i+1<input.size();++i)
        if(!expand_segment(input[i],input[i+1],factor,output.subspan(i*factor,factor)))return false;
    output.back()=input.back();return true;
}
}
bool PreparedResampler::prepare_window(const ResamplingWindowConfig& config,const ResamplerBackendFactory* factory) {
    if(!valid_resampling_window(config))return false;
    PreparedResampler next;next.window_config_=config;next.window_mode_=true;
    if(config.mode==ResamplingWindowMode::SincBestQuality){
        if(!factory)return false;
        next.backend_=factory->prepare_window(config);
        if(!next.backend_)return false;
        next.factory_=factory;
    }
    next.prepared_=true;
    next.reset();*this=std::move(next);return true;
}
bool PreparedResampler::window_resource_requirements(const ResamplingWindowConfig& config,
    const ResamplerBackendFactory* factory,ResamplerResources& resources) noexcept {
    resources={};
    if(!valid_resampling_window(config))return false;
    if(config.mode==ResamplingWindowMode::SincBestQuality &&
       (!factory || !factory->query_window(config,resources)))return false;
    if(resources.owned_bytes>SIZE_MAX-sizeof(PreparedResampler))return false;
    resources.owned_bytes+=sizeof(PreparedResampler);return true;
}
bool PreparedResampler::process_window(std::span<const float> input,std::size_t stride,
    std::span<float> output,std::size_t capacity,std::size_t& written) noexcept {
    written=0;
    if(window_mode_ && window_config_.mode==ResamplingWindowMode::EndpointLinear)
        return process_window_prefix(input,window_config_.input_samples,stride,output,capacity,written);
    if(!prepared_ || !window_mode_ || !backend_)return false;
    const auto& c=window_config_;
    if(stride<c.input_samples || capacity<c.output_samples || stride>input.size()/c.channels ||
        capacity>output.size()/c.channels || produced_>UINT64_MAX-c.output_samples)return false;
    if(!finite_window_channels(input,c.input_samples,stride,c.channels))return false;
    if(!backend_->process_window(input,stride,output,capacity,written) || written>c.output_samples ||
        !finite_window_channels(output,written,capacity,c.channels))return reject_output(written);
    produced_+=written;return true;
}
std::size_t PreparedResampler::window_output_samples(std::size_t count) const noexcept {
    if(!prepared_ || !window_mode_ || count>window_config_.input_samples)return 0;
    if(window_config_.mode==ResamplingWindowMode::SincBestQuality)
        return count==window_config_.input_samples?window_config_.output_samples:0;
    std::size_t output=0;
    return endpoint_expansion_size(count,window_config_.expansion_factor,output)?output:0;
}
bool PreparedResampler::process_window_prefix(std::span<const float> input,std::size_t count,std::size_t stride,
    std::span<float> output,std::size_t capacity,std::size_t& written) noexcept {
    written=0;
    if(!prepared_ || !window_mode_ || window_config_.mode!=ResamplingWindowMode::EndpointLinear)return false;
    ProcessBuffers buffers{input,count,stride,output,capacity,written};
    return process_endpoint_window(buffers);
}
bool PreparedResampler::process_endpoint_window(ProcessBuffers& buffers) noexcept {
    const auto& c=window_config_;const auto count=window_output_samples(buffers.count);
    if(buffers.count>c.input_samples || (buffers.count && !count) ||
       buffers.stride<buffers.count || buffers.capacity<count || buffers.stride>buffers.input.size()/c.channels ||
       buffers.capacity>buffers.output.size()/c.channels || produced_>UINT64_MAX-count)return false;
    if(!buffers.count)return true;
    const bool identity=c.expansion_factor==1 && buffers.stride==buffers.capacity;
    if(!compatible_buffers(buffers.input,buffers.output,identity) ||
       !finite_window_channels(buffers.input,buffers.count,buffers.stride,c.channels))return false;
    for(std::size_t channel=0;channel<c.channels;++channel)
        if(!expand_channel(buffers.input.subspan(channel*buffers.stride,buffers.count),c.expansion_factor,
            buffers.output.subspan(channel*buffers.capacity,count)))return reject_output(buffers.written);
    buffers.written=count;produced_+=count;return true;
}
}
