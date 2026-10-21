#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace dsp_kernels {
// Exact integer clock; finite FIR and fractional delay have measured error.
// Coefficients are prepared in double. Processing is exclusively Float32.
class PcmResampler {
public:
    enum class Quality { Full, Compact };
    static constexpr std::size_t taps=128, phases=128; // Base design sizes.
    struct Geometry {
        std::size_t taps=0, phases=0;
        std::uint32_t delay=0;
        std::size_t coefficient_bytes=0, state_bytes=0;
    };
    static bool supported(std::uint32_t input,std::uint32_t output) noexcept {
        return input>=8000 && input<=192000 && output>=8000 && output<=192000 &&
            std::uint64_t(input)<=std::uint64_t(output)*8 && std::uint64_t(output)<=std::uint64_t(input)*8;
    }
    static std::size_t maximum_output(std::size_t input,std::uint32_t in,std::uint32_t out) noexcept {
        if(!in || !out)return 0;
        if(input>(UINT64_MAX-(in-1))/out)return SIZE_MAX;
        const auto count=(std::uint64_t(input)*out+in-1)/in;
        return count>=SIZE_MAX ? SIZE_MAX : std::size_t(count)+1;
    }
    static Geometry geometry(std::uint32_t in,std::uint32_t out,std::size_t channels,
                             Quality quality=Quality::Full) noexcept {
        if(!supported(in,out) || !channels || channels>2 || in==out || (quality!=Quality::Full && quality!=Quality::Compact))return {};
        const std::uint64_t half=quality==Quality::Full?64:16;
        const auto delay=std::uint32_t((half*std::max(in,out)+out-1)/out);
        const auto count=std::size_t(delay)*2+2;
        const auto denominator=out/std::gcd(in,out);
        const auto phase_count=std::min<std::size_t>(denominator,quality==Quality::Full?128:32);
        return {count,phase_count,delay,(phase_count+1)*count*sizeof(float),2*channels*count*sizeof(float)};
    }
    static std::size_t storage_bytes(std::size_t channels,std::uint32_t in,std::uint32_t out,
                                    Quality quality=Quality::Full) noexcept {
        const auto g=geometry(in,out,channels,quality);return g.coefficient_bytes+g.state_bytes;
    }
    bool prepare(std::uint32_t input,std::uint32_t output,std::size_t channels,
                 Quality quality=Quality::Full) {
        if(!supported(input,output) || !channels || channels>2 || (quality!=Quality::Full && quality!=Quality::Compact))return false;
        PcmResampler next;next.configure(input,output,channels,quality);
        if(input!=output)next.prepare_coefficients();
        next.reset();*this=std::move(next);return true;
    }
    void reset() noexcept {
        std::fill(history_.begin(),history_.end(),0.f);position_=until_=0;fraction_=0;produced_=0;
        anchored_=input_expected_=has_input_=false;anchor_=next_input_=0;
    }
    bool copy_state_from(const PcmResampler& other) noexcept {
        if(input_!=other.input_ || output_!=other.output_ || channels_!=other.channels_ ||
           quality_!=other.quality_ || history_.size()!=other.history_.size())return false;
        std::copy(other.history_.begin(),other.history_.end(),history_.begin());
        position_=other.position_;until_=other.until_;fraction_=other.fraction_;produced_=other.produced_;
        anchor_=other.anchor_;anchored_=other.anchored_;next_input_=other.next_input_;
        input_expected_=other.input_expected_;has_input_=other.has_input_;return true;
    }
    // First output is numerator/output_rate input samples after the first input.
    // Only allowed before processing after prepare/reset.
    bool initial_position(std::uint32_t numerator) noexcept {
        if(!input_ || has_input_ || numerator>=input_)return false;
        until_=numerator/output_+std::size_t(numerator%output_!=0);fraction_=numerator%output_;return true;
    }
    const Geometry& filter_geometry() const noexcept {return geometry_;}
    std::uint32_t delay_input_samples() const noexcept {return geometry_.delay;}
    std::span<const float> coefficients() const noexcept {return weights_;}
    bool input_contiguous(std::int64_t ticks) const noexcept {return !input_expected_ || ticks==next_input_;}
    bool advance_input(std::int64_t ticks,std::size_t count) noexcept {
        if(!std::in_range<std::int64_t>(count) || ticks>INT64_MAX-std::int64_t(count))return false;
        next_input_=ticks+std::int64_t(count);input_expected_=true;return true;
    }
    bool anchored() const noexcept {return anchored_;}
    void anchor(std::int64_t ticks) noexcept {anchor_=ticks;anchored_=true;}
    std::int64_t anchor_ticks() const noexcept {return anchor_;}
    std::uint64_t produced() const noexcept {return produced_;}
    // Planar, disjoint input/output buffers. Mirrored history gives contiguous
    // newest-first dot products; all capacity is prepared before processing.
    // Validation failure preserves state; arithmetic failure resets it and
    // reports that reset to an owning wrapper through state_reset.
    bool process(std::span<const float> input,std::size_t count,std::size_t input_stride,
                 std::span<float> output,std::size_t output_stride,std::size_t& written,
                 bool* state_reset=nullptr) noexcept {
        if(state_reset)*state_reset=false;
        written=0;const auto bound=maximum_output(count,input_,output_);
        if(!input_||!channels_||input_stride<count||input_stride>input.size()/channels_||output_stride>output.size()/channels_||output_stride<bound||produced_>UINT64_MAX-bound)return false;
        if(!finite_input(input,count,input_stride))return false;
        if(!count)return true;
        has_input_=true;
        ProcessBuffers buffers{input,count,input_stride,output,output_stride,written,state_reset};
        if(input_==output_)return copy_identity(buffers);
        return process_filtered(buffers);
    }

private:
    struct Rotation {
        double sine,cosine,step_sine,step_cosine;
        void advance() noexcept {const auto next=sine*step_cosine+cosine*step_sine;cosine=cosine*step_cosine-sine*step_sine;sine=next;}
    };
    struct CoefficientDesign {double cutoff,sinc_step,window_step,sinc_sine,sinc_cosine,window_sine,window_cosine;};
    struct ProcessBuffers {
        std::span<const float> input;std::size_t count,input_stride;std::span<float> output;std::size_t output_stride;std::size_t& written;bool* state_reset;
    };
    void configure(std::uint32_t input,std::uint32_t output,std::size_t channels,Quality quality){
        input_=input;output_=output;channels_=channels;quality_=quality;integer_step_=input/output;fractional_step_=input%output;inverse_output_=1.f/float(output);
        geometry_=geometry(input,output,channels,quality);
    }
    CoefficientDesign coefficient_design()const noexcept {
        constexpr double pi=3.14159265358979323846;
        const double cutoff=(quality_==Quality::Full?.45:.43)*std::min(1.,double(output_)/input_);
        const double sinc_step=2.*pi*cutoff,window_step=pi/geometry_.delay;
        return {cutoff,sinc_step,window_step,std::sin(sinc_step),std::cos(sinc_step),std::sin(window_step),std::cos(window_step)};
    }
    double coefficient_value(double distance,double cutoff,double sine,double window_cosine)const noexcept {
        constexpr double pi=3.14159265358979323846;const double x=2.*cutoff*distance;
        const double sinc=x==0?1.:sine/(pi*x);const double window=std::abs(distance)<geometry_.delay?.5+.5*window_cosine:0.;
        return 2.*cutoff*sinc*window;
    }
    void prepare_phase(std::size_t phase,const CoefficientDesign& design){
        const auto& g=geometry_;const double fraction=double(phase)/g.phases,initial=-double(g.delay)-fraction;
        Rotation sinc{std::sin(design.sinc_step*initial),std::cos(design.sinc_step*initial),design.sinc_sine,design.sinc_cosine};
        Rotation window{std::sin(design.window_step*initial),std::cos(design.window_step*initial),design.window_sine,design.window_cosine};
        double sum=0;
        for(std::size_t k=0;k<g.taps;++k){
            const double value=coefficient_value(double(k)-g.delay-fraction,design.cutoff,sinc.sine,window.cosine);
            weights_[phase*g.taps+k]=float(value);sum+=value;sinc.advance();window.advance();
        }
        for(std::size_t k=0;k<g.taps;++k)weights_[phase*g.taps+k]=float(weights_[phase*g.taps+k]/sum);
    }
    void prepare_coefficients(){
        weights_.resize((geometry_.phases+1)*geometry_.taps);history_.resize(2*channels_*geometry_.taps);
        const auto design=coefficient_design();for(std::size_t phase=0;phase<=geometry_.phases;++phase)prepare_phase(phase,design);
    }
    bool finite_input(std::span<const float> input,std::size_t count,std::size_t stride)const noexcept {
        for(std::size_t channel=0;channel<channels_;++channel)for(std::size_t i=0;i<count;++i)if(!std::isfinite(input[channel*stride+i]))return false;
        return true;
    }
    bool copy_identity(ProcessBuffers& buffers)noexcept {
        for(std::size_t channel=0;channel<channels_;++channel)std::memcpy(buffers.output.data()+channel*buffers.output_stride,buffers.input.data()+channel*buffers.input_stride,buffers.count*sizeof(float));
        buffers.written=buffers.count;produced_+=buffers.count;return true;
    }
    void push_sample(const ProcessBuffers& buffers,std::size_t index)noexcept {
        const auto width=geometry_.taps;
        for(std::size_t channel=0;channel<channels_;++channel){auto* history=history_.data()+channel*2*width;history[position_]=history[position_+width]=buffers.input[channel*buffers.input_stride+index];}
    }
    bool reject_output(ProcessBuffers& buffers)noexcept {reset();if(buffers.state_reset)*buffers.state_reset=true;buffers.written=0;return false;}
    void advance_phase()noexcept {
        const auto old=fraction_;auto next=fraction_+fractional_step_;const auto carry=std::uint32_t(next>=output_);if(carry)next-=output_;
        until_=std::size_t(integer_step_+carry)+(next!=0)-(old!=0);fraction_=next;
    }
    struct PhaseWeights {const float* first;const float* second;float blend;};
    PhaseWeights phase_weights()const noexcept {
        const auto backwards=fraction_?output_-fraction_:0u,scaled=backwards*std::uint32_t(geometry_.phases);
        const auto phase=std::size_t(scaled/output_);const auto* first=weights_.data()+phase*geometry_.taps;
        return {first,first+geometry_.taps,float(scaled%output_)*inverse_output_};
    }
    bool store_filtered_channel(ProcessBuffers& buffers,std::size_t channel,const PhaseWeights& weights)noexcept {
        const auto* samples=history_.data()+channel*2*geometry_.taps+position_;
        const float value=dot(samples,weights.first,weights.second,geometry_.taps,weights.blend);
        if(!std::isfinite(value))return false;
        buffers.output[channel*buffers.output_stride+buffers.written]=value;return true;
    }
    bool emit_phase(ProcessBuffers& buffers)noexcept {
        const auto weights=phase_weights();
        for(std::size_t channel=0;channel<channels_;++channel)if(!store_filtered_channel(buffers,channel,weights))return reject_output(buffers);
        ++buffers.written;++produced_;advance_phase();return true;
    }
    bool process_filtered(ProcessBuffers& buffers)noexcept {
        for(std::size_t i=0;i<buffers.count;++i){
            push_sample(buffers,i);
            while(until_==0)if(!emit_phase(buffers))return false;
            --until_;position_=position_?position_-1:geometry_.taps-1;
        }
        return true;
    }
    static void accumulate_quad(std::array<float,4>& sums,const float* x,const float* weights,std::size_t index)noexcept {
        for(std::size_t lane=0;lane<4;++lane)sums[lane]+=x[index+lane]*weights[index+lane];
    }
    static float sum_quad(const std::array<float,4>& sums)noexcept {return (sums[0]+sums[1])+(sums[2]+sums[3]);}
    static float dot_single(const float* x,const float* weights,std::size_t size)noexcept {
        std::array<float,4> sums{};std::size_t index=0;
        for(;index+4<=size;index+=4)accumulate_quad(sums,x,weights,index);
        float value=sum_quad(sums);for(;index<size;++index)value+=x[index]*weights[index];return value;
    }
    static std::array<float,2> dot_pair(const float* x,const float* a,const float* b,std::size_t size)noexcept {
        std::array<float,4> left{},right{};std::size_t index=0;
        for(;index+4<=size;index+=4){accumulate_quad(left,x,a,index);accumulate_quad(right,x,b,index);}
        float first=sum_quad(left),second=sum_quad(right);
        for(;index<size;++index){first+=x[index]*a[index];second+=x[index]*b[index];}return {first,second};
    }
    #if defined(__GNUC__) || defined(__clang__)
    [[gnu::always_inline]]
    #endif
    static inline float dot(const float* x,const float* a,const float* b,std::size_t size,float blend)noexcept {
        if(blend==0.f)return dot_single(x,a,size);
        const auto values=dot_pair(x,a,b,size);return values[0]+blend*(values[1]-values[0]);
    }
    std::vector<float> weights_,history_;
    Geometry geometry_{};
    Quality quality_=Quality::Full;
    std::uint32_t input_=0,output_=0,fraction_=0;
    std::uint32_t integer_step_=0,fractional_step_=0;
    float inverse_output_=0;
    std::size_t channels_=0,position_=0,until_=0;
    std::uint64_t produced_=0;
    std::int64_t anchor_=0,next_input_=0;
    bool anchored_=false,input_expected_=false,has_input_=false;
};
}
