#include <embedded_support/session.h>
#include <algorithm>
namespace embedded_support {
namespace {
bool terminal(Status s) noexcept {return s!=Status::ok && s!=Status::would_block;}
}
namespace nd=bdsp_protocol::numeric_data;
NumericSession::NumericSession(Role role,const NumericSessionBinding& binding,NumericSessionBuffers buffers,nd::Limits limits,
    PacketSender sender,void* sender_user,std::size_t packet_payload_capacity) noexcept
    :role_(role),sender_(sender),sender_user_(sender_user),packet_payload_capacity_(packet_payload_capacity),binding_(binding),buffers_(buffers),limits_(limits),packets_(buffers.packet_in,buffers.packet_out),assembler_(buffers.assembly) {
    packets_.set_packet_handler([](bdsp_protocol::PacketView packet,void* user){static_cast<NumericSession*>(user)->packet(packet);},this);
}
Status NumericSession::begin() noexcept {
    stop();
    const auto& b=binding_;
    const auto* numeric=std::get_if<signal_contracts::NumericContract>(&b.binding.contract);
    std::uint8_t encoding=0;
    if(b.negotiated.payload!=nd::PayloadVersion || !(b.negotiated.features&unsigned(nd::Feature::Numeric)) ||
       !b.nonce || !b.session || !b.publisher_epoch || !b.binding.reference.binding || !b.binding.reference.contract || b.binding.endpoint.graph.empty() || b.binding.endpoint.node<0 ||
       b.binding.stream.empty() || (!sender_ && buffers_.read.empty()) ||
       signal_contracts::validate_contract(b.binding.contract,limits_.frame)!=Status::ok ||
       !numeric || !nd::symbol_id(numeric->representation.encoding,encoding) ||
       (role_==Role::publisher && (buffers_.transmit_frame.size()<limits_.message_bytes || buffers_.encode_scratch.size()<limits_.message_bytes)))
        return fail(Status::incompatible_contract);
    if(packet_payload_capacity_<=8 || packet_payload_capacity_>65535)return fail(Status::invalid_argument);
    const auto chunk=std::min(bdsp_protocol::StreamFragmentBytes,packet_payload_capacity_-8);
    const auto fragment=std::min(limits_.message_bytes,chunk)+8;
    if(!limits_.message_bytes || limits_.message_bytes>bdsp_protocol::MaxFrameBytes ||
       buffers_.fragment.size()<fragment || (!sender_ && (buffers_.packet_in.size()<fragment+4 ||
       buffers_.packet_out.size()<2*fragment+16)) || (role_==Role::subscriber && buffers_.assembly.size()<limits_.message_bytes))
        return fail(Status::buffer_too_small);
    nd::Message marker;marker.kind=nd::Kind::Unavailable;marker.contract=b.binding.contract;marker.endpoint=b.binding.endpoint;
    marker.stamp.available=false;marker.stamp.stream_id=b.binding.stream;marker.values=b.binding.values;
    marker.envelope={b.nonce,b.session,1,b.publisher_epoch,0,b.revision,0};marker.reference=b.binding.reference;
    const auto metadata=nd::measure(marker,limits_,{b.nonce,b.session,b.revision,std::span(&b.binding,1)});
    if(!metadata)return fail(metadata.status);
    if(role_==Role::subscriber && (buffers_.decode.text.size()<metadata.required.text ||
       buffers_.decode.channels.size()<metadata.required.channels || buffers_.decode.planes.size()<numeric->channel_order.size()))
        return fail(Status::buffer_too_small);
    state_=SessionState::ready;return Status::ok;
}
void NumericSession::stop() noexcept {
    packets_.reset();assembler_.reset();tx_size_=tx_offset_=0;sequence_=received_=0;gap_=true;
    state_=SessionState::stopped;last_=Status::ok;
}
Status NumericSession::fail(Status s) noexcept {
    packets_.reset();assembler_.reset();tx_size_=tx_offset_=0;state_=SessionState::failed;return last_=s;
}
bool NumericSession::matches(const nd::Message& m) const noexcept {
    const auto& b=binding_.binding;
    return (m.kind==nd::Kind::Frame || m.kind==nd::Kind::Unavailable) && m.endpoint.graph==b.endpoint.graph &&
        m.endpoint.node==b.endpoint.node && m.endpoint.direction==b.endpoint.direction && m.endpoint.port==b.endpoint.port &&
        m.stamp.stream_id==b.stream && m.values==b.values && signal_contracts::same_contract(m.contract,b.contract);
}
Status NumericSession::publish(const nd::Message& input) noexcept {
    if(role_!=Role::publisher || state_!=SessionState::ready)return Status::unavailable;
    if(!matches(input))return Status::incompatible_contract;
    if(tx_size_ || packets_.pending_bytes()) {
        ++dropped_;gap_=true;return input.stamp.available?Status::overflow:fail(Status::overflow);
    }
    if(sequence_==UINT64_MAX)return fail(Status::exhausted);
    auto m=input;m.envelope.nonce=binding_.nonce;m.envelope.session=binding_.session;m.envelope.revision=binding_.revision;
    m.envelope.delivery_epoch=binding_.publisher_epoch;m.envelope.delivery_sequence=sequence_+1;m.stamp.discontinuity|=gap_;
    m.reference=binding_.binding.reference;
    const nd::ReferenceContext context{binding_.nonce,binding_.session,binding_.revision,std::span(&binding_.binding,1)};
    auto result=nd::encode(m,std::as_writable_bytes(buffers_.transmit_frame),std::as_writable_bytes(buffers_.encode_scratch),limits_,context);
    if(!result)return result.status;
    ++sequence_;tx_size_=result.produced;tx_offset_=0;gap_=false;return pump();
}
Status NumericSession::pump() noexcept {
    if(!tx_size_ || packets_.pending_bytes())return Status::ok;
    auto result=bdsp_protocol::encode_fragment(buffers_.transmit_frame.first(tx_size_),tx_offset_,buffers_.fragment,
        std::min(bdsp_protocol::StreamFragmentBytes,packet_payload_capacity_-8));
    if(!result)return fail(result.status);
    const auto fragment=buffers_.fragment.first(result.written_bytes);
    auto s=sender_?sender_(sender_user_,nd::FragmentId,fragment):packets_.enqueue(nd::FragmentId,fragment);
    if(s==Status::would_block)return s;
    if(s!=Status::ok)return fail(s);
    tx_offset_+=result.written_bytes-8;if(tx_offset_==tx_size_)tx_size_=tx_offset_=0;return Status::ok;
}
Status NumericSession::accept_packet(bdsp_protocol::PacketView incoming) noexcept {
    if(!sender_ || state_==SessionState::stopped)return Status::unavailable;
    if(state_==SessionState::failed)return last_;
    packet(incoming);return state_==SessionState::failed?last_:Status::ok;
}
Status NumericSession::drain_packets() noexcept {
    if(!sender_ || state_==SessionState::stopped)return Status::unavailable;
    if(state_==SessionState::failed)return last_;
    return pump();
}
void NumericSession::packet(bdsp_protocol::PacketView packet) noexcept {
    if(state_!=SessionState::ready)return;
    if(packet.payload.size()>packet_payload_capacity_){fail(Status::buffer_too_small);return;}
    if(role_!=Role::subscriber || packet.id!=nd::FragmentId){fail(Status::malformed);return;}
    std::span<const std::uint8_t> complete;auto s=assembler_.accept(packet.payload,complete);
    if(s==Status::would_block)return;
    if(s!=Status::ok){fail(s);return;}
    nd::Message message;const nd::ReferenceContext context{binding_.nonce,binding_.session,binding_.revision,std::span(&binding_.binding,1)};
    auto result=nd::decode(std::as_bytes(complete),message,buffers_.decode,limits_,context);
    if(!result){fail(result.status);return;}
    if(!matches(message) || message.envelope.nonce!=binding_.nonce || message.envelope.session!=binding_.session ||
       message.envelope.revision!=binding_.revision || message.envelope.delivery_epoch!=binding_.publisher_epoch ||
       !message.envelope.delivery_sequence){fail(Status::incompatible_contract);return;}
    if(message.envelope.delivery_sequence<=received_)return;
    message.stamp.discontinuity|=gap_ || (received_ && message.envelope.delivery_sequence-received_!=1);
    received_=message.envelope.delivery_sequence;gap_=!message.stamp.available;
    if(handler_)handler_(user_,message);
}
Status NumericSession::poll(bdsp_protocol::ByteTransport& transport) noexcept {
    if(sender_)return Status::unavailable;
    if(state_==SessionState::stopped)return Status::unavailable;
    if(state_==SessionState::failed)return last_;
    if(transport.state()!=bdsp_protocol::TransportState::connected)return fail(Status::disconnected);
    auto s=pump();if(terminal(s))return s;
    auto sent=packets_.flush(transport);if(terminal(sent.status))return fail(sent.status);
    auto read=transport.read(buffers_.read);if(read.transferred>buffers_.read.size())return fail(Status::invalid_argument);
    if(read.transferred){s=packets_.feed(buffers_.read.first(read.transferred));if(state_==SessionState::failed)return last_;if(terminal(s))return fail(s);}
    if(terminal(read.status))return fail(read.status);
    return Status::ok;
}
Status DeliveryFencedRuntime::apply(application::PreparedExecution&& prepared) {
    const auto status=executor_.apply(std::move(prepared));if(status==Status::ok && fence_)fence_(user_);return status;
}
Status DeliveryFencedRuntime::shutdown(){const auto status=executor_.shutdown();if(status==Status::ok && fence_)fence_(user_);return status;}
}
