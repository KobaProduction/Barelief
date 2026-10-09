#include "signal_contracts/frame.h"
#include <cmath>
#include <limits>
#include <array>
namespace signal_contracts {
namespace {
constexpr FrameDescriptor descriptors[]{
    {FrameType::PCM,"audio",DataShape::Vector,true,Quantity::Amplitude,Unit::NormalizedAmplitude},
    {FrameType::Window,"window",DataShape::Vector,true,Quantity::Amplitude,Unit::NormalizedAmplitude},
    {FrameType::Windowed,"windowed",DataShape::Vector,true,Quantity::Amplitude,Unit::NormalizedAmplitude},
    {FrameType::StereoSpectrum,"stereo-spectrum",DataShape::Vector,true,Quantity::SpectrumMagnitude,Unit::Unspecified},
    {FrameType::Spectrum,"spectrum",DataShape::Vector,false,Quantity::SpectrumMagnitude,Unit::Unspecified},
    {FrameType::Mel,"mel",DataShape::Vector,false,Quantity::MelEnergy,Unit::Unspecified},
    {FrameType::History,"history",DataShape::Matrix,false,Quantity::MelEnergy,Unit::Unspecified},
    {FrameType::Scalar,"scalar",DataShape::Scalar,false,Quantity::Unspecified,Unit::Unspecified},
    {FrameType::WledPacket,"wled-packet",DataShape::Packet,false,Quantity::Packet,Unit::Bytes}
};
}
std::span<const FrameDescriptor> frame_descriptors() noexcept { return descriptors; }
const FrameDescriptor* frame_descriptor(FrameType t) noexcept {
    for (const auto& d:descriptors) if(d.type==t) return &d;
    return nullptr;
}
std::string_view type_id(FrameType t) noexcept {
    const auto* d=frame_descriptor(t);return d?d->id:std::string_view{};
}
bool compatible(const PortContract &a, const PortContract &b) noexcept { return a == b; }
PortContract contract(const FrameView &f) noexcept {
    const auto &m=f.metadata;
    return {f.type,m.sample_rate,m.fft_size,m.bands,m.history_capacity,
        frame_descriptor(f.type) && frame_descriptor(f.type)->stereo?f.left.size():f.type==FrameType::WledPacket?f.packet.size():f.type==FrameType::History?f.history.bands:f.values.size(),
        m.frequency_step,m.bark_weighted};
}
Status validate(const FrameView &f) noexcept {
    const auto &m=f.metadata; const auto* descriptor=frame_descriptor(f.type);
    const bool has_history_descriptor=!f.history.storage.empty() || f.history.bands ||
        f.history.frames || f.history.capacity || f.history.oldest || !f.history.newest_override.empty() ||
        !f.history.zero_rows.empty() || !f.history.zero_row.empty() || f.history.zero_oldest;
    if(!descriptor || (f.type!=FrameType::WledPacket && !f.packet.empty()) || m.sample_rate>768000 || m.fft_size>65536 || m.bands>4096 ||
       m.history_capacity>2000 || m.stream_id.size()>256 || !std::isfinite(m.frequency_step) ||
       m.frequency_step<0 || !std::isfinite(m.peak_hz) || !std::isfinite(m.magnitude)) return Status::invalid_argument;
    if(!m.available) return !m.sequence && f.left.empty() && f.right.empty() && f.values.empty() &&
        !has_history_descriptor && f.packet.empty()?Status::unavailable:Status::invalid_argument;
    if(!m.sequence || !m.sample_rate) return Status::invalid_argument;
    auto finite=[](auto values){for(float v:values) if(!std::isfinite(v))return false;return true;};
    if(descriptor->stereo) {
        if(f.left.empty() || f.left.size()!=f.right.size() || !f.values.empty() ||
           has_history_descriptor || !finite(f.left) || !finite(f.right))return Status::invalid_argument;
        if(f.type!=FrameType::PCM && m.fft_size && f.left.size() != (f.type==FrameType::StereoSpectrum?m.fft_size/2:m.fft_size))return Status::invalid_argument;
    } else if(f.type==FrameType::WledPacket) {
        if(f.packet.size()!=44 || !f.left.empty() || !f.right.empty() || !f.values.empty() || has_history_descriptor ||
           m.fft_size || m.bands || m.history_capacity || m.frequency_step || m.bark_weighted)return Status::invalid_argument;
    } else if(f.type==FrameType::Scalar) {
        if(f.values.size()!=1 || !f.left.empty() || !f.right.empty() || has_history_descriptor || !finite(f.values) ||
           m.fft_size || m.bands || m.history_capacity || m.frequency_step || m.bark_weighted)return Status::invalid_argument;
    } else if(f.type==FrameType::Spectrum || f.type==FrameType::Mel) {
        if(f.values.empty() || !f.left.empty() || !f.right.empty() || has_history_descriptor ||
           !finite(f.values) || (f.type==FrameType::Mel && m.bands && f.values.size()!=m.bands) ||
           (f.type==FrameType::Spectrum && m.fft_size && f.values.size()!=m.fft_size/2))return Status::invalid_argument;
    } else {
        const auto &h=f.history;
        if(!f.left.empty() || !f.right.empty() || !f.values.empty() || !h.bands || !h.frames ||
           !h.capacity || h.frames>h.capacity || h.oldest>=h.capacity ||
           h.bands>std::numeric_limits<std::size_t>::max()/h.capacity || h.storage.size()!=h.bands*h.capacity ||
           (!h.newest_override.empty() && h.newest_override.size()!=h.bands) ||
           (h.zero_rows.empty()!=h.zero_row.empty()) ||
           (h.zero_rows.empty() && h.zero_oldest) ||
           (!h.zero_rows.empty() && (h.zero_rows.size()<h.frames || h.zero_rows.size()>2000 ||
                                    h.zero_oldest>=h.zero_rows.size() || h.zero_row.size()!=h.bands)) ||
           (m.bands && m.bands!=h.bands) || (m.history_capacity && h.capacity>m.history_capacity))return Status::invalid_argument;
        for(auto flag:h.zero_rows)if(flag>1)return Status::invalid_argument;
        for(float value:h.zero_row)if(value!=0)return Status::invalid_argument;
        for(std::size_t i=0;i<h.frames;++i) if(!finite(h.row(i))) return Status::invalid_argument;
    }
    return Status::ok;
}
}
