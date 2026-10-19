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
bool history_present(const HistoryView& history) noexcept {
    return !history.storage.empty() || history.bands || history.frames || history.capacity || history.oldest || !history.newest_override.empty() || !history.zero_rows.empty() || !history.zero_row.empty() || history.zero_oldest;
}
bool metadata_bounded(const FrameMetadata& metadata) noexcept {
    return metadata.sample_rate<=768000 && metadata.fft_size<=65536 && metadata.bands<=4096 && metadata.history_capacity<=2000 && metadata.stream_id.size()<=256 && std::isfinite(metadata.frequency_step) && metadata.frequency_step>=0 && std::isfinite(metadata.peak_hz) && std::isfinite(metadata.magnitude);
}
bool finite_samples(std::span<const float> samples) noexcept {
    for(const auto sample:samples)if(!std::isfinite(sample))return false;
    return true;
}
bool scalar_metadata(const FrameMetadata& metadata) noexcept {
    return !metadata.fft_size && !metadata.bands && !metadata.history_capacity && !metadata.frequency_step && !metadata.bark_weighted;
}
Status stereo_frame(const FrameView& frame,bool history) noexcept {
    if(frame.left.empty() || frame.left.size()!=frame.right.size() || !frame.values.empty() || history || !finite_samples(frame.left) || !finite_samples(frame.right))return Status::invalid_argument;
    if(frame.type!=FrameType::PCM && frame.metadata.fft_size && frame.left.size()!=(frame.type==FrameType::StereoSpectrum?frame.metadata.fft_size/2:frame.metadata.fft_size))return Status::invalid_argument;
    return Status::ok;
}
Status packet_frame(const FrameView& frame,bool history) noexcept {
    return frame.packet.size()==44 && frame.left.empty() && frame.right.empty() && frame.values.empty() && !history && scalar_metadata(frame.metadata)?Status::ok:Status::invalid_argument;
}
Status scalar_frame(const FrameView& frame,bool history) noexcept {
    return frame.values.size()==1 && frame.left.empty() && frame.right.empty() && !history && finite_samples(frame.values) && scalar_metadata(frame.metadata)?Status::ok:Status::invalid_argument;
}
Status feature_frame(const FrameView& frame,bool history) noexcept {
    if(frame.values.empty() || !frame.left.empty() || !frame.right.empty() || history || !finite_samples(frame.values))return Status::invalid_argument;
    if(frame.type==FrameType::Mel && frame.metadata.bands && frame.values.size()!=frame.metadata.bands)return Status::invalid_argument;
    if(frame.type==FrameType::Spectrum && frame.metadata.fft_size && frame.values.size()!=frame.metadata.fft_size/2)return Status::invalid_argument;
    return Status::ok;
}
bool history_storage_valid(const FrameView& frame) noexcept {
    const auto& history=frame.history;
    if(!frame.left.empty() || !frame.right.empty() || !frame.values.empty() || !history.bands || !history.frames || !history.capacity || history.frames>history.capacity || history.oldest>=history.capacity)return false;
    if(history.bands>std::numeric_limits<std::size_t>::max()/history.capacity || history.storage.size()!=history.bands*history.capacity)return false;
    return (!frame.metadata.bands || frame.metadata.bands==history.bands) && (!frame.metadata.history_capacity || history.capacity<=frame.metadata.history_capacity);
}
bool history_overrides_valid(const HistoryView& history) noexcept {
    if(!history.newest_override.empty() && history.newest_override.size()!=history.bands)return false;
    if(history.zero_rows.empty()!=history.zero_row.empty())return false;
    if(history.zero_rows.empty())return history.zero_oldest==0;
    return history.zero_rows.size()>=history.frames && history.zero_rows.size()<=2000 && history.zero_oldest<history.zero_rows.size() && history.zero_row.size()==history.bands;
}
bool history_samples_valid(const HistoryView& history) noexcept {
    for(const auto flag:history.zero_rows)if(flag>1)return false;
    for(const auto value:history.zero_row)if(value!=0)return false;
    for(std::size_t row=0;row<history.frames;++row)if(!finite_samples(history.row(row)))return false;
    return true;
}
Status history_frame(const FrameView& frame) noexcept {
    const auto& history=frame.history;
    if(!history_storage_valid(frame) || !history_overrides_valid(history))return Status::invalid_argument;
    return history_samples_valid(history)?Status::ok:Status::invalid_argument;
}
Status frame_payload(const FrameView& frame,const FrameDescriptor& descriptor,bool history) noexcept {
    if(descriptor.stereo)return stereo_frame(frame,history);
    switch(frame.type) {
    case FrameType::WledPacket:return packet_frame(frame,history);
    case FrameType::Scalar:return scalar_frame(frame,history);
    case FrameType::Spectrum:case FrameType::Mel:return feature_frame(frame,history);
    default:return history_frame(frame);
    }
}
Status unavailable_frame(const FrameView& frame,bool history) noexcept {
    return !frame.metadata.sequence && frame.left.empty() && frame.right.empty() && frame.values.empty() && !history && frame.packet.empty()?Status::unavailable:Status::invalid_argument;
}
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
Status validate(const FrameView& frame) noexcept {
    const auto* descriptor=frame_descriptor(frame.type);
    const bool history=history_present(frame.history);
    if(!descriptor || (frame.type!=FrameType::WledPacket && !frame.packet.empty()) || !metadata_bounded(frame.metadata))return Status::invalid_argument;
    if(!frame.metadata.available)return unavailable_frame(frame,history);
    if(!frame.metadata.sequence || !frame.metadata.sample_rate)return Status::invalid_argument;
    return frame_payload(frame,*descriptor,history);
}
}
