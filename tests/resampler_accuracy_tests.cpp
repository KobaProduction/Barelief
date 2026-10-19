#include <dsp_kernels/pcm_resampler.h>
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <vector>

using dsp_kernels::PcmResampler;
namespace {
constexpr double pi=std::numbers::pi;
std::vector<float> process(PcmResampler& op,const std::vector<float>& input,unsigned in,unsigned out,std::size_t packet){
    std::vector<float> result;result.reserve(PcmResampler::maximum_output(input.size(),in,out));
    std::vector<float> buffer(PcmResampler::maximum_output(packet,in,out));
    for(std::size_t at=0;at<input.size();){const auto count=std::min(packet,input.size()-at);std::size_t written=0;
        assert(op.process(std::span(input).subspan(at,count),count,count,buffer,buffer.size(),written));
        result.insert(result.end(),buffer.begin(),buffer.begin()+written);at+=count;}
    return result;
}
struct Fit {double amplitude=0,relative_residual=0,quadrature=0;};
Fit fit(const std::vector<float>& samples,unsigned rate,double frequency,std::size_t begin,double input_delay=0){
    double ss=0,cc=0,sc=0,ys=0,yc=0;const auto end=std::min(samples.size(),begin+rate);
    for(auto i=begin;i<end;++i){const double angle=2*pi*frequency*(double(i)/rate-input_delay),s=std::sin(angle),c=std::cos(angle);
        ss+=s*s;cc+=c*c;sc+=s*c;ys+=samples[i]*s;yc+=samples[i]*c;}
    const auto determinant=ss*cc-sc*sc;assert(determinant>1);
    const auto a=(ys*cc-yc*sc)/determinant,b=(yc*ss-ys*sc)/determinant;
    double error=0,power=0;for(auto i=begin;i<end;++i){const auto angle=2*pi*frequency*(double(i)/rate-input_delay);
        const auto residual=samples[i]-a*std::sin(angle)-b*std::cos(angle);error+=residual*residual;power+=double(samples[i])*samples[i];}
    return {std::hypot(a,b),std::sqrt(error/std::max(power,1e-30)),b};
}
void tone_quality(PcmResampler::Quality quality,unsigned in,unsigned out,double frequency,bool stopband=false){
    PcmResampler op;assert(op.prepare(in,out,1,quality));std::vector<float> input(2*in);
    for(std::size_t i=0;i<input.size();++i)input[i]=float(std::sin(2*pi*frequency*i/in));
    const auto output=process(op,input,in,out,509);
    const double alias=std::abs(frequency-out*std::round(frequency/out));
    const auto result=fit(output,out,alias,out,stopband?0:double(op.delay_input_samples())/in);
    const auto gain_db=20*std::log10(std::max(result.amplitude,1e-30));
    const auto residual_db=20*std::log10(std::max(result.relative_residual,1e-30));
    std::printf("FIR %s %u->%u tone=%.1f gain=%.5fdB residual=%.2fdBc delay=%u taps=%zu phases=%zu bytes=%zu\n",
        quality==PcmResampler::Quality::Full?"full":"compact",in,out,frequency,gain_db,residual_db,
        op.delay_input_samples(),op.filter_geometry().taps,op.filter_geometry().phases,PcmResampler::storage_bytes(1,in,out,quality));
    std::fflush(stdout);
    if(stopband)assert(gain_db<(quality==PcmResampler::Quality::Full?-60.:-40.));
    else {assert(std::abs(gain_db)<.1);assert(std::abs(result.quadrature)<.0002);
        assert(residual_db<(quality==PcmResampler::Quality::Full?-80.:-66.));}
}
void convolution_oracle(PcmResampler::Quality quality,unsigned in,unsigned out,unsigned initial=0){
    PcmResampler op;assert(op.prepare(in,out,1,quality));std::vector<float> input(2003);
    assert(op.initial_position(initial));
    for(std::size_t i=0;i<input.size();++i){double value=0;for(unsigned k=0;k<7;++k)value+=.06*std::sin((.01+.017*k)*i+.3*k);input[i]=float(value);}
    const auto result=process(op,input,in,out,97);const auto g=op.filter_geometry();const auto weights=op.coefficients();double maximum=0;
    // Direct indexed convolution with double accumulation and multiplication;
    // rational output time is computed independently for every sample.
    for(std::size_t m=0;m<result.size();++m){const auto numerator=std::uint64_t(m)*in+initial;
        const auto index=std::size_t((numerator+out-1)/out);
        const auto back=std::uint32_t(index*out-numerator);const auto scaled=std::uint64_t(back)*g.phases;
        const auto phase=std::size_t(scaled/out);const double fraction=double(scaled%out)/out;
        double expected=0;for(std::size_t k=0;k<g.taps;++k)if(k<=index){
            const auto coefficient=(1-fraction)*weights[phase*g.taps+k]+fraction*weights[(phase+1)*g.taps+k];expected+=input[index-k]*coefficient;}
        maximum=std::max(maximum,std::abs(expected-result[m]));
    }
    std::printf("Float32/direct-double oracle %u->%u initial=%u max_abs=%.9g\n",in,out,initial,maximum);assert(maximum<2e-6);
    for(std::size_t p=0;p<=g.phases;++p){double sum=0;for(std::size_t k=0;k<g.taps;++k)sum+=weights[p*g.taps+k];assert(std::abs(sum-1)<2e-7);}
}
}
int main(){
    for(const auto quality:{PcmResampler::Quality::Full,PcmResampler::Quality::Compact}){
        for(const auto in:{48000u,44100u})for(const auto out:{32768u,32000u,36000u}){
            for(const auto initial:{0u,1u,in/2,in-1})convolution_oracle(quality,in,out,initial);
            tone_quality(quality,in,out,9900.);
        }
        for(const auto out:{8000u,8001u}){tone_quality(quality,64000,out,900.);tone_quality(quality,64000,out,std::ceil(.501*out),true);}
        tone_quality(quality,8000,64000,900.);
        for(const auto rates:{std::pair{64000u,8001u},std::pair{192000u,24000u},std::pair{8000u,64000u},std::pair{48017u,32749u}})
            for(const auto initial:{1u,rates.first-1})convolution_oracle(quality,rates.first,rates.second,initial);
    }
    std::puts("resampler_accuracy_tests PASS: direct double oracle, DC, passband/phase/residual, 8:1 stopband, upsampling");
}
