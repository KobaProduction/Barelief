#include "audio_decoder.h"
#include <algorithm>
#define MA_API static
#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
// miniaudio's lock is an implementation global rather than an MA_API symbol.
// Keep this private decoding implementation separate from the capture adapter.
#define ma_atomic_global_lock qtvis_music_decoder_atomic_global_lock
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

namespace desktop_analysis::offline {
struct AudioDecoder::Impl {
    ma_decoder decoder{};bool opened=false;ma_uint64 frames=0;
    dsp_kernels::ChannelPolicy policy=dsp_kernels::ChannelPolicy::Mean;
    std::uint32_t selected=0;std::vector<float> interleaved;
    ~Impl(){if(opened)ma_decoder_uninit(&decoder);}
    bool initialize(const QString& path,QString& error){
        auto config=ma_decoder_config_init(ma_format_f32,0,0);
#ifdef _WIN32
        const auto native=path.toStdWString();const auto status=ma_decoder_init_file_w(native.c_str(),&config,&decoder);
#else
        const auto native=path.toUtf8();const auto status=ma_decoder_init_file(native.constData(),&config,&decoder);
#endif
        if(status!=MA_SUCCESS){error=QString::fromUtf8(ma_result_description(status));return false;}
        opened=true;return true;
    }
    bool configure_channels(dsp_kernels::ChannelPolicy requested,std::uint32_t index,QString& error){
        if(!decoder.outputChannels||decoder.outputChannels>256||
           (requested==dsp_kernels::ChannelPolicy::Select&&index>=decoder.outputChannels)){
            error="Invalid analysis channel selection";return false;
        }
        policy=requested;selected=index;interleaved.resize(std::size_t(decoder.outputChannels)*4096);
        ma_decoder_get_length_in_pcm_frames(&decoder,&frames);return true;
    }
    float mono_sample(std::span<const float> values)const {
        if(policy==dsp_kernels::ChannelPolicy::Select)return values[selected];
        double sum=0;for(float value:values)sum+=value;return float(sum/values.size());
    }
    void copy_mono(std::span<float> mono,std::size_t count)const {
        const auto channels=decoder.outputChannels;
        for(std::size_t frame=0;frame<count;++frame)mono[frame]=mono_sample(std::span(interleaved).subspan(frame*channels,channels));
    }
};
AudioDecoder::AudioDecoder():impl_(std::make_unique<Impl>()){}
AudioDecoder::~AudioDecoder()=default;
bool AudioDecoder::open(const QString& path,QString& error,dsp_kernels::ChannelPolicy policy,std::uint32_t channel_index) {
    if(policy!=dsp_kernels::ChannelPolicy::Mean && policy!=dsp_kernels::ChannelPolicy::Select){error="Invalid channel policy";return false;}
    auto next=std::make_unique<Impl>();
    if(!next->initialize(path,error)||!next->configure_channels(policy,channel_index,error))return false;
    impl_=std::move(next);return true;
}
bool AudioDecoder::read(std::span<float> mono,std::size_t& frames,QString& error) {
    frames=0;if(!impl_->opened){error="Decoder is not open";return false;}ma_uint64 count=0;
    const auto requested=std::min<std::size_t>(mono.size(),4096);
    const auto status=ma_decoder_read_pcm_frames(&impl_->decoder,impl_->interleaved.data(),requested,&count);frames=std::size_t(count);
    impl_->copy_mono(mono,frames);
    if(status!=MA_SUCCESS&&status!=MA_AT_END){error=QString::fromUtf8(ma_result_description(status));return false;}return true;
}
std::uint32_t AudioDecoder::sample_rate()const{return impl_->opened?impl_->decoder.outputSampleRate:0;}
std::uint64_t AudioDecoder::length()const{return impl_->frames;}
}
