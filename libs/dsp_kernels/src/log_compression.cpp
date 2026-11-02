// Formula references: CPJKU/madmom@27f032e8947204902c675e5e341a3faf5dc86dae,
// audio/spectrogram.py LogarithmicSpectrogram; aubio cvec_logmag.
#include <dsp_kernels/log_compression.h>
#include <cmath>
#include <functional>
namespace dsp_kernels {
namespace {
bool partial_overlap(std::span<const float> input,std::span<float> output) noexcept {
    if(input.empty() || input.data()==output.data())return false;
    const std::less<const float*> less;
    return less(input.data(),output.data()+output.size()) && less(output.data(),input.data()+input.size());
}
float logarithm(float input,LogarithmBase base) noexcept {
    if(base==LogarithmBase::Natural)return std::log(input);
    return base==LogarithmBase::Two?std::log2(input):std::log10(input);
}
bool valid(std::span<const float> input,std::span<float> output,float multiply,float add,bool allow_signed) noexcept {
    if(input.size()!=output.size() || partial_overlap(input,output) || !std::isfinite(multiply) || multiply<0 || !std::isfinite(add) || add<0)return false;
    for(float value:input)
        if(!std::isfinite(value) || (!allow_signed && value<0) || add+multiply*value<0 || !std::isfinite(add+multiply*value))return false;
    return true;
}
}
bool logarithmic_compression(std::span<const float> input,std::span<float> output,float multiply,float add,LogarithmBase base,bool allow_signed) noexcept {
    if(base<LogarithmBase::Ten || base>LogarithmBase::Two || !valid(input,output,multiply,add,allow_signed))return false;
    for(std::size_t i=0;i<input.size();++i)output[i]=logarithm(add+multiply*input[i],base);
    return true;
}
bool logarithmic_magnitude(std::span<const float> input,std::span<float> output,float lambda) noexcept {
    return logarithmic_compression(input,output,lambda,1,LogarithmBase::Natural);
}
}
