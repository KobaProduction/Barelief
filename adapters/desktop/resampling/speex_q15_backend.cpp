#include "speex_backend.h"
#define OUTSIDE_SPEEX
#define RANDOM_PREFIX qtvis_speex_q15
#include "vendor/speex_resampler.h"
#include <cstring>
extern "C" {
std::size_t qtvis_speex_q15_coefficient_bytes(const SpeexResamplerState*);
std::size_t qtvis_speex_q15_state_bytes(const SpeexResamplerState*);
int qtvis_speex_q15_copy_state(SpeexResamplerState*,const SpeexResamplerState*);
void qtvis_speex_q15_reset(SpeexResamplerState*);
int qtvis_speex_q15_initial_position(SpeexResamplerState*,unsigned,unsigned);
int qtvis_speex_q15_query(unsigned,unsigned,unsigned,unsigned,std::size_t*,std::size_t*,unsigned*);
}
namespace desktop_resampling {
namespace {
class Q15Backend final:public dsp_kernels::ResamplerBackend {
    dsp_kernels::ResamplerConfig config;
    SpeexResamplerState* state=nullptr;
public:
    explicit Q15Backend(const dsp_kernels::ResamplerConfig& c):config(c) {
        int error=0;state=speex_resampler_init(unsigned(c.channels),c.input_rate,c.output_rate,int(c.quality),&error);
        if(error && state){speex_resampler_destroy(state);state=nullptr;}
    }
    ~Q15Backend() override {if(state)speex_resampler_destroy(state);}
    bool valid() const noexcept {return state!=nullptr;}
    bool process(std::span<const float>,std::size_t,std::size_t,std::span<float>,std::size_t,std::size_t&) noexcept override {return false;}
    bool process_pcm16(std::span<const std::int16_t> input,std::size_t count,std::size_t stride,
        std::span<std::int16_t> output,std::size_t capacity,std::size_t& written) noexcept override {
        written=0;if(!state || count>UINT32_MAX || capacity>UINT32_MAX)return false;
        if(config.input_rate==config.output_rate && capacity<count)return false;
        if(!count)return true;
        if(config.input_rate==config.output_rate){copy_identity(input,count,stride,output,capacity);written=count;return true;}
        return process_channels(input,count,stride,output,capacity,written);
    }
private:
    void copy_identity(std::span<const std::int16_t> input,std::size_t count,std::size_t stride,
        std::span<std::int16_t> output,std::size_t capacity) noexcept {
        for(std::size_t c=0;c<config.channels;++c)
            std::memcpy(output.data()+c*capacity,input.data()+c*stride,count*sizeof(std::int16_t));
    }
    bool process_channels(std::span<const std::int16_t> input,std::size_t count,std::size_t stride,
        std::span<std::int16_t> output,std::size_t capacity,std::size_t& written) noexcept {
        for(unsigned c=0;c<config.channels;++c) {
            unsigned consumed=unsigned(count),produced=unsigned(capacity);
            if(speex_resampler_process_int(state,c,input.data()+c*stride,&consumed,output.data()+c*capacity,&produced))return false;
            if(consumed!=count || (c && written!=produced))return false;
            written=produced;
        }
        return true;
    }
public:
    void reset() noexcept override {if(state)qtvis_speex_q15_reset(state);}
    bool initial_position(std::uint32_t n,std::uint32_t d) noexcept override {return state && qtvis_speex_q15_initial_position(state,n,d);}
    bool copy_state_from(const dsp_kernels::ResamplerBackend& other) noexcept override {
        const auto* source=dynamic_cast<const Q15Backend*>(&other);
        return source && config==source->config && state && source->state && qtvis_speex_q15_copy_state(state,source->state);
    }
    dsp_kernels::ResamplerResources resources() const noexcept override {
        dsp_kernels::ResamplerResources result;if(!state)return result;
        result.coefficient_bytes=qtvis_speex_q15_coefficient_bytes(state);result.state_bytes=qtvis_speex_q15_state_bytes(state);
        result.owned_bytes=sizeof(*this)+result.coefficient_bytes+result.state_bytes;
        result.fixed_delay_input_samples=config.input_rate==config.output_rate?0:unsigned(speex_resampler_get_input_latency(state));return result;
    }
};
}
bool query_speex_q15(const dsp_kernels::ResamplerConfig& c,dsp_kernels::ResamplerResources& r) noexcept {
    r={};if(c.mode!=dsp_kernels::ResamplerMode::SpeexDspQ15 || !dsp_kernels::PreparedResampler::supported(c.input_rate,c.output_rate) || !c.channels || c.channels>2 || c.quality>10)return false;
    unsigned delay=0;
    if(!qtvis_speex_q15_query(c.input_rate,c.output_rate,unsigned(c.channels),c.quality,&r.coefficient_bytes,&r.state_bytes,&delay))return false;
    r.owned_bytes=sizeof(Q15Backend)+r.coefficient_bytes+r.state_bytes;r.fixed_delay_input_samples=delay;return true;
}
std::unique_ptr<dsp_kernels::ResamplerBackend> prepare_speex_q15(const dsp_kernels::ResamplerConfig& c) {
    dsp_kernels::ResamplerResources r;if(!query_speex_q15(c,r))return {};
    auto result=std::make_unique<Q15Backend>(c);if(!result->valid())return {};return result;
}
}
