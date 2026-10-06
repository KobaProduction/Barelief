#include "bdsp_protocol/byte_stream.h"
#include <BDSP/transmitter.h>
#include <BDSP/streams/writer.h>
#include <BDSP/strategies/cobs/write.h>
#include <array>
#include <cstring>
namespace bdsp_protocol {
using signal_contracts::Status;
namespace {
// Reuse upstream algorithm with an embedded scratch block. Upstream's default strategy
// allocates this block; initializing it before set_strategy and detaching on destruction
// keeps the unchanged framing algorithm usable without an allocator/global manager swap.
class BoundedZpeWriter final : public BDSP::strategies::core::COBSZPEWriteStrategyCore {
public:
    BoundedZpeWriter() noexcept { _cfg=get_default_config();_buffer_ptr=scratch_.data(); }
    ~BoundedZpeWriter() override { _buffer_ptr=nullptr; }
private:
    std::array<std::uint8_t,224> scratch_{};
};
struct Checksum : BDSP::core::BDSPV1ChecksumMixin {using BDSPV1ChecksumMixin::_calc_checksum;};
std::size_t write_packet(std::uint8_t id,std::span<const std::uint8_t> payload,std::span<std::uint8_t> out) noexcept {
    struct Sink {std::span<std::uint8_t> bytes;std::size_t used=0;} sink{out};
    BoundedZpeWriter strategy;BDSP::streams::core::StreamWriterCore writer;
    writer.set_strategy(strategy);
    writer.set_stream_writer([](uint8_t byte,void *ctx){auto &s=*static_cast<Sink *>(ctx);if(s.used<s.bytes.size())s.bytes[s.used]=byte;++s.used;},&sink);
    BDSP::core::TransmitterV1Core transmitter;transmitter.set_stream_writer(&writer);
    transmitter.send_data(id,const_cast<std::uint8_t *>(payload.data()),payload.size());
    return sink.used;
}
}
CodecResult encode_packet(std::uint8_t id,std::span<const std::uint8_t> payload,std::span<std::uint8_t> out) noexcept {
    if(id>31 || payload.empty() || payload.size()>65535)return {Status::invalid_argument};
    const auto required=write_packet(id,payload,{});
    if(out.size()<required)return {Status::buffer_too_small,required};
    return {Status::ok,required,write_packet(id,payload,out)};
}
PacketStream::PacketStream(std::span<std::uint8_t> incoming,std::span<std::uint8_t> outgoing) noexcept
    :incoming_(incoming),outgoing_(outgoing) {
    reader_.set_strategy(strategy_);
    reader_.set_stream_data_handler([](uint8_t byte,BDSP::streams::stream_read_status_t status,void *ctx){static_cast<PacketStream *>(ctx)->on_byte(byte,status);},this);
}
void PacketStream::reset() noexcept {
    reader_.reset_read_state(false);incoming_size_=outgoing_size_=outgoing_offset_=0;discarding_=false;feed_status_=Status::ok;
}
Status PacketStream::feed(std::span<const std::uint8_t> bytes) noexcept {
    feed_status_=Status::ok;
    for(auto byte:bytes) {
        const auto status=reader_.read(byte);
        if(status==BDSP::streams::STREAM_READ_ERROR || status==BDSP::streams::STREAM_READER_NOT_READY_ERROR) {
            feed_status_=Status::malformed;discarding_=true;
        }
    }
    return feed_status_;
}
void PacketStream::on_byte(std::uint8_t byte,BDSP::streams::stream_read_status_t status) noexcept {
    if(status==BDSP::streams::STREAM_READ_ERROR || status==BDSP::streams::STREAM_READER_NOT_READY_ERROR) {
        feed_status_=Status::malformed;discarding_=true;return;
    }
    if(status!=BDSP::streams::STREAM_READ_DELIMITER) {
        if(discarding_)return;
        if(incoming_size_==incoming_.size()){discarding_=true;feed_status_=Status::overflow;++overflows_;return;}
        incoming_[incoming_size_++]=byte;return;
    }
    if(discarding_){discarding_=false;incoming_size_=0;return;}
    const auto count=incoming_size_;incoming_size_=0;
    if(!count)return; // Ignore the delimiter used to resynchronize after errors.
    auto malformed=[&]{feed_status_=Status::malformed;};
    BDSP::core::packet_v1_header header{};std::memcpy(&header,incoming_.data(),1);
    if(header.is_unsupported_protocol_version){feed_status_=Status::unsupported_version;return;}
    const std::size_t start=header.is_two_bytes_for_packet_size?3:2;
    if(count<start){malformed();return;}
    const std::size_t size=header.is_two_bytes_for_packet_size?(std::size_t(incoming_[1])<<8)|incoming_[2]:incoming_[1];
    if(!size || count!=start+size+(header.is_checksum_used?1:0)){malformed();return;}
    if(header.is_checksum_used && Checksum{}._calc_checksum(header,incoming_.data()+start,size)!=incoming_[count-1]){malformed();return;}
    if(handler_)handler_({std::uint8_t(header.packet_id),incoming_.subspan(start,size)},context_);
}
Status PacketStream::enqueue(std::uint8_t id,std::span<const std::uint8_t> payload) noexcept {
    if(pending_bytes()){++overflows_;return Status::overflow;}
    const auto result=encode_packet(id,payload,outgoing_);
    if(!result){if(result.status==Status::buffer_too_small)++overflows_;return result.status;}
    outgoing_size_=result.written_bytes;outgoing_offset_=0;return Status::ok;
}
IoResult PacketStream::flush(ByteTransport &transport) noexcept {
    if(!pending_bytes())return {Status::ok,0};
    if(transport.state()!=TransportState::connected)return {Status::disconnected,0};
    auto result=transport.write(outgoing_.subspan(outgoing_offset_,pending_bytes()));
    if(result.transferred>pending_bytes())return {Status::invalid_argument,0};
    outgoing_offset_+=result.transferred;
    if(outgoing_offset_==outgoing_size_)outgoing_offset_=outgoing_size_=0;
    return result;
}
}
