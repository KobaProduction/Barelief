#include "speex_backend.h"
#define OUTSIDE_SPEEX
#define RANDOM_PREFIX qtvis_speex
#include "vendor/speex_resampler.h"
#include <limits>
#include <cstring>
extern "C" {
std::size_t qtvis_speex_coefficient_bytes(const SpeexResamplerState*);
std::size_t qtvis_speex_state_bytes(const SpeexResamplerState*);
int qtvis_speex_copy_state(SpeexResamplerState*,const SpeexResamplerState*);
void qtvis_speex_reset(SpeexResamplerState*);
int qtvis_speex_initial_position(SpeexResamplerState*,unsigned,unsigned);
int qtvis_speex_query(unsigned,unsigned,unsigned,unsigned,std::size_t*,std::size_t*,unsigned*);
}
namespace desktop_resampling {
namespace {
class SpeexBackend final:public dsp_kernels::ResamplerBackend {
public:
    explicit SpeexBackend(const dsp_kernels::ResamplerConfig& c):config_(c) {
        int error=0;state_=speex_resampler_init(unsigned(c.channels),c.input_rate,c.output_rate,int(c.quality),&error);
        if(error && state_){speex_resampler_destroy(state_);state_=nullptr;}
    }
    ~SpeexBackend() override {if(state_)speex_resampler_destroy(state_);}
    bool valid() const noexcept {return state_!=nullptr;}
    void reset() noexcept override {if(state_)qtvis_speex_reset(state_);}
    bool initial_position(std::uint32_t numerator,std::uint32_t denominator) noexcept override {
        return state_ && qtvis_speex_initial_position(state_,numerator,denominator);
    }
    bool copy_state_from(const dsp_kernels::ResamplerBackend& src) noexcept override {
        const auto* other=dynamic_cast<const SpeexBackend*>(&src);
        return other && state_ && other->state_ && config_==other->config_ && qtvis_speex_copy_state(state_,other->state_);
    }
    dsp_kernels::ResamplerResources resources() const noexcept override {
        dsp_kernels::ResamplerResources r;if(!state_)return r;
        r.coefficient_bytes=qtvis_speex_coefficient_bytes(state_);r.state_bytes=qtvis_speex_state_bytes(state_);
        r.owned_bytes=sizeof(*this)+r.coefficient_bytes+r.state_bytes;
        r.fixed_delay_input_samples=config_.input_rate==config_.output_rate?0:unsigned(speex_resampler_get_input_latency(state_));return r;
    }
    bool process(std::span<const float> input,std::size_t count,std::size_t stride,
        std::span<float> output,std::size_t capacity,std::size_t& written) noexcept override {
        written=0;if(!state_ || count>UINT32_MAX || capacity>UINT32_MAX)return false;
        if(!count)return true;
        // Equal rate is an explicit identity for every mode in this interface.
        if(config_.input_rate==config_.output_rate){
            copy_identity(input,count,stride,output,capacity);
            written=count;return true;
        }
        return process_channels(input,count,stride,output,capacity,written);
    }
private:
    void copy_identity(std::span<const float> input,std::size_t count,std::size_t stride,std::span<float> output,std::size_t capacity) noexcept {
        for(std::size_t c=0;c<config_.channels;++c)
            std::memcpy(output.data()+c*capacity,input.data()+c*stride,count*sizeof(float));
    }
    bool process_channels(std::span<const float> input,std::size_t count,std::size_t stride,std::span<float> output,std::size_t capacity,std::size_t& written) noexcept {
        for(std::size_t c=0;c<config_.channels;++c){unsigned consumed=unsigned(count),produced=unsigned(capacity);
            const int error=speex_resampler_process_float(state_,unsigned(c),input.data()+c*stride,&consumed,output.data()+c*capacity,&produced);
            if(error || consumed!=count || (c && written!=produced))return false;
            written=produced;
        }
        return true;
    }
private:
    dsp_kernels::ResamplerConfig config_;SpeexResamplerState* state_=nullptr;
};
class SpeexFactory final:public dsp_kernels::ResamplerBackendFactory {
public:
    bool query(const dsp_kernels::ResamplerConfig& c,dsp_kernels::ResamplerResources& r) const noexcept override {
        r={};if(c.mode!=dsp_kernels::ResamplerMode::SpeexDsp || !dsp_kernels::PcmResampler::supported(c.input_rate,c.output_rate) || !c.channels || c.channels>2 || c.quality>10)return false;
        unsigned delay=0;
        if(!qtvis_speex_query(c.input_rate,c.output_rate,unsigned(c.channels),c.quality,&r.coefficient_bytes,&r.state_bytes,&delay))return false;
        r.fixed_delay_input_samples=delay;
        r.owned_bytes=sizeof(SpeexBackend)+r.coefficient_bytes+r.state_bytes;return true;
    }
    std::unique_ptr<dsp_kernels::ResamplerBackend> prepare(const dsp_kernels::ResamplerConfig& c) const override {
        if(c.mode!=dsp_kernels::ResamplerMode::SpeexDsp || !dsp_kernels::PcmResampler::supported(c.input_rate,c.output_rate) ||
            !c.channels || c.channels>2 || c.quality>10)return {};
        auto result=std::make_unique<SpeexBackend>(c);if(!result->valid())return {};return result;
    }
};
}
const dsp_kernels::ResamplerBackendFactory& speex_backend_factory() noexcept {static const SpeexFactory factory;return factory;}
}
