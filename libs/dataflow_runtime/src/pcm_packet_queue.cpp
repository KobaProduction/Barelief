#include <dataflow_runtime/pcm_packet_queue.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dataflow_runtime {
using namespace signal_contracts;
bool PcmPacketQueue::prepare(std::uint32_t rate,std::size_t samples,std::size_t packets){
    if(rate<8000 || rate>192000 || !samples || samples>192000 || !packets || packets>4096)return false;
    samples_.assign(samples,0);packets_.assign(packets,{});rate_=rate;
    sample_head_=sample_count_=packet_head_=packet_count_=0;
    accepted_=overflow_=dropped_=discontinuities_=sequence_=0;last_={};marker_=false;pending_gap_=true;return true;
}
void PcmPacketQueue::clear() noexcept {
    dropped_+=sample_count_;sample_head_=sample_count_=packet_head_=packet_count_=0;pending_gap_=true;marker_=false;
}
Status PcmPacketQueue::push(std::span<const float> input,const FrameStamp& stamp) noexcept {
    if(!rate_ || input.empty() || !stamp.available || !stamp.has_sample_interval || stamp.sample_count!=input.size() ||
       stamp.sample_begin!=stamp.timestamp_ticks || !std::in_range<std::int64_t>(input.size()) || stamp.sample_begin>INT64_MAX-std::int64_t(input.size()))return Status::invalid_argument;
    for(const auto value:input)if(!std::isfinite(value))return Status::invalid_argument;
    if(marker_ && stamp.producer_epoch==last_.producer_epoch && stamp.generation==last_.generation && stamp.sequence<=last_.sequence)return Status::would_block;
    const bool gap=pending_gap_ || stamp.discontinuity || (marker_ && (stamp.producer_epoch!=last_.producer_epoch || stamp.generation!=last_.generation ||
        last_.sample_count>std::uint64_t(INT64_MAX) || last_.sample_begin>INT64_MAX-std::int64_t(last_.sample_count) || stamp.sample_begin!=last_.sample_begin+std::int64_t(last_.sample_count)));
    // Overflow rejects the incoming packet atomically and declares a gap. The
    // next accepted packet starts a clean queue rather than emitting stale PCM.
    if(input.size()>samples_.size() || (!gap && (input.size()>samples_.size()-sample_count_ || packet_count_==packets_.size()))){
        ++overflow_;dropped_+=input.size();pending_gap_=true;return Status::exhausted;
    }
    if(gap){dropped_+=sample_count_;sample_head_=sample_count_=packet_head_=packet_count_=0;++discontinuities_;}
    const auto tail=(sample_head_+sample_count_)%samples_.size();
    for(std::size_t i=0;i<input.size();++i)samples_[(tail+i)%samples_.size()]=input[i];
    auto& packet=packets_[(packet_head_+packet_count_)%packets_.size()];packet={stamp,input.size(),0};packet.stamp.stream_id={};packet.stamp.discontinuity=gap;
    sample_count_+=input.size();++packet_count_;accepted_+=input.size();last_=stamp;last_.stream_id={};marker_=true;pending_gap_=false;return Status::ok;
}
Status PcmPacketQueue::pop(std::span<float> output,std::size_t& written,FrameStamp& stamp) noexcept {
    written=0;if(!rate_ || output.empty())return Status::invalid_argument;if(!packet_count_)return Status::would_block;
    if(sequence_==UINT64_MAX)return Status::overflow;
    auto& packet=packets_[packet_head_];const auto count=std::min(output.size(),packet.size-packet.consumed);
    for(std::size_t i=0;i<count;++i)output[i]=samples_[(sample_head_+i)%samples_.size()];
    stamp=packet.stamp;if(!stamp.source_sequence)stamp.source_sequence=stamp.sequence;stamp.sequence=++sequence_;
    stamp.timestamp_ticks=stamp.sample_begin+=std::int64_t(packet.consumed);stamp.sample_count=count;
    stamp.discontinuity=packet.stamp.discontinuity && packet.consumed==0;
    sample_head_=(sample_head_+count)%samples_.size();sample_count_-=count;packet.consumed+=count;
    if(packet.consumed==packet.size){packet_head_=(packet_head_+1)%packets_.size();--packet_count_;}
    written=count;return Status::ok;
}
PcmPacketQueueStats PcmPacketQueue::stats() const noexcept {return {sample_count_,packet_count_,samples_.size(),packets_.size(),accepted_,overflow_,dropped_,discontinuities_};}
std::size_t PcmPacketQueue::storage_bytes(std::size_t samples,std::size_t packets) noexcept {
    if(samples>SIZE_MAX/sizeof(float) || packets>(SIZE_MAX-samples*sizeof(float))/sizeof(Packet))return SIZE_MAX;
    return samples*sizeof(float)+packets*sizeof(Packet);
}
std::size_t PcmPacketQueue::workspace_bytes() const noexcept {return samples_.capacity()*sizeof(float)+packets_.capacity()*sizeof(Packet);}
}
