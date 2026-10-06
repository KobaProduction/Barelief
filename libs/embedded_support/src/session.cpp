#include <embedded_support/session.h>
#include <algorithm>
#include <cstring>
namespace embedded_support {
namespace {
// Reject malformed UTF-8 just as the desktop round-trip check does.
bool utf8(std::string_view s) noexcept {
    for(std::size_t i=0;i<s.size();) {
        auto c=static_cast<unsigned char>(s[i++]);
        if(c<128)continue;
        unsigned n=0, value=0, minimum=0;
        if(c>=0xc2 && c<=0xdf){n=1;value=c&31;minimum=0x80;}
        else if(c>=0xe0 && c<=0xef){n=2;value=c&15;minimum=0x800;}
        else if(c>=0xf0 && c<=0xf4){n=3;value=c&7;minimum=0x10000;}
        else return false;
        if(n>s.size()-i)return false;
        while(n--){c=static_cast<unsigned char>(s[i++]);if((c&0xc0)!=0x80)return false;value=(value<<6)|(c&63);}
        if(value<minimum || value>0x10ffff || (value>=0xd800 && value<=0xdfff))return false;
    }
    return true;
}
bool terminal(Status s) {return s!=Status::ok && s!=Status::would_block;}
}
Session::Session(Role role, signal_contracts::FrameType type, std::string_view id, SessionBuffers buffers) noexcept
    :role_(role),type_(type),id_(id),buffers_(buffers),packets_(buffers.packet_in,buffers.packet_out),assembler_(buffers.assembly) {
    if(!id.empty() && id.size()<=256 && unsigned(type)<=6 && utf8(id)) {
        request_[0]=0;request_[1]=2;request_[2]=std::uint8_t(type);
        std::memcpy(request_.data()+3,id.data(),id.size());request_size_=id.size()+3;
    }
    packets_.set_packet_handler([](bdsp_protocol::PacketView p, void *ctx){static_cast<Session *>(ctx)->packet(p);},this);
}
Status Session::begin(std::uint64_t now, std::uint64_t epoch) noexcept {
    stop();
    if(!request_size_ || buffers_.read.empty() || buffers_.packet_in.size()<request_size_+4 ||
       buffers_.packet_out.size()<2*request_size_+16 ||
       (role_==Role::publisher && (!epoch || buffers_.transmit_frame.size()<112+id_.size() || buffers_.fragment.size()<120+id_.size())) ||
       (role_==Role::subscriber && (buffers_.assembly.size()<112+id_.size() || buffers_.decoded_id.size()<id_.size())))
        return fail(Status::invalid_argument);
    started_=now;epoch_=epoch;state_=SessionState::handshake;
    if(role_==Role::subscriber) {
        const auto s=packets_.enqueue(11,std::span(request_).first(request_size_));
        if(s!=Status::ok)return fail(s);
    }
    return last_=Status::ok;
}
void Session::stop() noexcept {
    packets_.reset();assembler_.reset();tx_size_=tx_offset_=0;
    rx_epoch_=rx_generation_=rx_sequence_=0;gap_=true;
    rx_producer_=0;rx_identity_known_=false;retired_count_=retired_next_=0;
    state_=SessionState::stopped;last_=Status::ok;
}
Status Session::fail(Status s) noexcept {
    packets_.reset();assembler_.reset();tx_size_=tx_offset_=0;
    state_=SessionState::failed;return last_=s;
}
void Session::packet(bdsp_protocol::PacketView p) noexcept {
    if(state_==SessionState::failed || state_==SessionState::stopped || state_==SessionState::rejecting)return;
    auto match=[&]{return p.payload.size()==request_size_ && std::equal(p.payload.begin(),p.payload.end(),request_.begin());};
    if(role_==Role::publisher) {
        if(state_!=SessionState::handshake || p.id!=11 || !match()) {
            // Existing typed-v2 explicit error, no new control message or wire format.
            constexpr std::string_view error="Unsupported stream/version/type";
            packets_.reset();tx_size_=tx_offset_=0;
            auto s=packets_.enqueue(14,{reinterpret_cast<const std::uint8_t *>(error.data()),error.size()});
            if(s!=Status::ok){fail(s);return;}
            state_=SessionState::rejecting;last_=Status::incompatible_contract;return;
        }
        auto s=packets_.enqueue(12,p.payload);
        if(s!=Status::ok){fail(s);return;}
        state_=SessionState::ready;gap_=true;return;
    }
    if(p.id==14){fail(Status::incompatible_contract);return;}
    if(state_==SessionState::handshake) {
        if(p.id!=12 || !match()){fail(Status::malformed);return;}
        state_=SessionState::ready;return;
    }
    if(p.id!=13){fail(Status::malformed);return;}
    std::span<const std::uint8_t> complete;
    auto s=assembler_.accept(p.payload,complete);
    if(s==Status::would_block)return;
    if(s!=Status::ok){fail(s);return;}
    signal_contracts::FrameView frame;
    const auto decoded=bdsp_protocol::decode(complete,buffers_.samples,buffers_.decoded_id,frame);
    if(!decoded){fail(decoded.status);return;}
    const auto valid=signal_contracts::validate(frame);
    if(frame.type!=type_ || frame.metadata.stream_id!=id_ || (valid!=Status::ok && valid!=Status::unavailable)){fail(Status::incompatible_contract);return;}
    const auto &m=frame.metadata;
    ProducerIdentity incoming{m.epoch,m.producer_epoch?m.producer_epoch:m.epoch};
    // A source-less clearing marker belongs to the last identity in this binding;
    // its zero generation does not lower the known generation or sequence fence.
    if(!m.available && !m.producer_epoch && !m.generation && rx_identity_known_ && m.epoch==rx_epoch_)
        incoming.producer=rx_producer_;
    const ProducerIdentity current{rx_epoch_,rx_producer_};
    const bool same_producer=rx_identity_known_ && incoming==current;
    if(!same_producer && std::find(retired_.begin(),retired_.begin()+retired_count_,incoming)!=retired_.begin()+retired_count_)return;
    if(same_producer && m.generation<rx_generation_ && (m.available || m.generation))return;
    const bool same_generation=same_producer && (m.generation==rx_generation_ || (!m.available && !m.generation));
    if(m.available && same_generation && m.sequence<=rx_sequence_)return;
    frame.metadata.discontinuity |= gap_ || !same_generation ||
        (m.available && rx_sequence_ && m.sequence-rx_sequence_!=1);
    if(!same_producer && rx_identity_known_) {
        retired_[retired_next_]=current;
        retired_next_=(retired_next_+1)%retired_.size();
        retired_count_=std::min(retired_count_+1,retired_.size());
    }
    if(m.available || !same_generation)rx_sequence_=m.sequence;
    rx_epoch_=incoming.publisher;rx_producer_=incoming.producer;rx_identity_known_=true;
    if(m.available || !same_generation || m.generation)rx_generation_=m.generation;
    gap_=!m.available;
    if(handler_)handler_(user_,frame);
}
Status Session::publish(const signal_contracts::FrameView &input) noexcept {
    if(role_!=Role::publisher || state_!=SessionState::ready)return Status::unavailable;
    if(input.type!=type_ || input.metadata.stream_id!=id_)return Status::incompatible_contract;
    const auto valid=signal_contracts::validate(input);
    if(valid!=Status::ok && valid!=Status::unavailable)return Status::invalid_argument;
    if(tx_size_ || packets_.pending_bytes()) {
        ++dropped_;gap_=true;
        // A partially written packet cannot be replaced without breaking framing.
        // Close/reset instead of allowing stale data to survive a marker.
        if(!input.metadata.available)return fail(Status::overflow);
        return Status::overflow;
    }
    auto frame=input;frame.metadata.epoch=epoch_;frame.metadata.discontinuity|=gap_;
    const auto encoded=bdsp_protocol::encode(frame,buffers_.transmit_frame);
    if(!encoded)return encoded.status;
    const auto fragment_bytes=std::min(encoded.written_bytes,bdsp_protocol::StreamFragmentBytes)+8;
    if(buffers_.fragment.size()<fragment_bytes || buffers_.packet_out.size()<2*fragment_bytes+16)return Status::buffer_too_small;
    tx_size_=encoded.written_bytes;tx_offset_=0;gap_=false;
    const auto s=pump();if(s!=Status::ok)return fail(s);return s;
}
Status Session::pump() noexcept {
    if(packets_.pending_bytes() || !tx_size_)return Status::ok;
    const auto result=bdsp_protocol::encode_fragment(buffers_.transmit_frame.first(tx_size_),tx_offset_,buffers_.fragment);
    if(!result)return result.status;
    const auto s=packets_.enqueue(13,buffers_.fragment.first(result.written_bytes));
    if(s!=Status::ok)return s;
    tx_offset_+=result.written_bytes-8;
    if(tx_offset_==tx_size_)tx_size_=tx_offset_=0;
    return Status::ok;
}
Status Session::poll(bdsp_protocol::ByteTransport &transport, std::uint64_t now) noexcept {
    if(state_==SessionState::stopped)return Status::unavailable;
    if(state_==SessionState::failed)return last_;
    if(transport.state()!=bdsp_protocol::TransportState::connected)return fail(Status::disconnected);
    if(state_==SessionState::handshake && now-started_>=5000)return fail(Status::unavailable);
    auto s=pump();if(terminal(s))return fail(s);
    auto sent=packets_.flush(transport);if(terminal(sent.status))return fail(sent.status);
    if(state_==SessionState::rejecting) {
        if(!packets_.pending_bytes())return fail(last_);
        return Status::would_block;
    }
    const auto received=transport.read(buffers_.read);
    if(received.transferred>buffers_.read.size())return fail(Status::invalid_argument);
    if(received.transferred) {
        s=packets_.feed(buffers_.read.first(received.transferred));
        if(state_==SessionState::failed)return last_;
        if(terminal(s))return fail(s);
    }
    if(terminal(received.status))return fail(received.status);
    return Status::ok;
}
}
