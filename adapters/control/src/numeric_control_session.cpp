#include <bdsp_protocol/version_admission.h>
#include <control/numeric_control_session.h>
#include <control/numeric_processing_message_mapper.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <vector>
namespace control::numeric {
namespace nc=bdsp_protocol::numeric_control;
namespace {
using signal_contracts::Status;
using namespace bdsp_protocol;
constexpr std::uint16_t HelloVersion=2;
std::size_t fragment_chunk(std::size_t packet) {return std::min(ControlFragmentBytes,packet-ControlFragmentHeaderBytes);}
bool data_packet(std::uint8_t id) {
 return id==bdsp_protocol::numeric_data::EventsId||id==bdsp_protocol::numeric_data::SnapshotId||id==bdsp_protocol::numeric_data::FragmentId;
}
bool add(std::size_t& total,std::size_t n) noexcept {
 if(n>std::numeric_limits<std::size_t>::max()-total)return false;
 total+=n;return true;
}
bool product(std::size_t& total,std::size_t count,std::size_t size) noexcept {
 if(size && count>std::numeric_limits<std::size_t>::max()/size)return false;
 return add(total,count*size);
}
struct StagingLease {
 const SessionConfig& config;std::span<std::byte> bytes;bool acquired=false,valid=false;
 StagingLease(const SessionConfig& c,std::span<std::byte> fallback):config(c),bytes(fallback) {
  if(c.acquire_staging) {
   bytes={};acquired=c.acquire_staging(c.staging_context,c.codec_limits.arena_bytes,bytes)==Status::ok;
   if(!acquired)return;
  }
  valid=bytes.size()>=c.codec_limits.arena_bytes&&bytes.data()&&
    reinterpret_cast<std::uintptr_t>(bytes.data())%alignof(std::max_align_t)==0;
  if(valid)bytes=bytes.first(c.codec_limits.arena_bytes);
 }
 ~StagingLease(){if(acquired)config.release_staging(config.staging_context);}
 StagingLease(const StagingLease&)=delete;
 StagingLease& operator=(const StagingLease&)=delete;
};
bool overlaps(std::span<const std::byte> a,std::span<const std::byte> b) noexcept {
 if(a.empty()||b.empty())return false;
 const auto first=reinterpret_cast<std::uintptr_t>(a.data()),second=reinterpret_cast<std::uintptr_t>(b.data());
 return first<=second?second-first<a.size():first-second<b.size();
}
bool text_valid(std::string_view text) {
 nc::Message m; nc::Command c; c.id=nc::CommandId::Apply; c.graph_id=text; c.token=1; m.body=c;
 return text.size()>=1 && text.size()<=256 && bool(nc::validate(m,nc::desktop_limits()));
}
void put(std::span<std::uint8_t> b,std::size_t& at,std::uint64_t n,unsigned width) {
 for(unsigned i=0;i<width;++i)b[at++]=std::uint8_t(n>>(8*i));
}
struct Reader {
 std::span<const std::uint8_t> bytes;std::size_t at=0;bool valid=true;
 std::uint64_t number(unsigned width) {
  if(width>bytes.size()-at){valid=false;return 0;}
  std::uint64_t n=0;for(unsigned i=0;i<width;++i)n|=std::uint64_t(bytes[at++])<<(8*i);return n;
 }
 std::string_view text(std::size_t maximum,bool empty=false) {
  const auto n=number(2);if(!valid||n>maximum||n>bytes.size()-at){valid=false;return {};}
  std::string_view value(reinterpret_cast<const char*>(bytes.data()+at),std::size_t(n));at+=std::size_t(n);
  if((!empty || !value.empty()) && !text_valid(value))valid=false;
  return value;
 }
 bool end() const {return valid && at==bytes.size();}
};
void put_text(std::span<std::uint8_t> out,std::size_t& at,std::string_view text) {
 put(out,at,text.size(),2);if(!text.empty()){std::memcpy(out.data()+at,text.data(),text.size());at+=text.size();}
}
nc::Error reply_error(application::ControlError error) {
 switch(error) {
 case application::ControlError::none:return nc::Error::None;
 case application::ControlError::conflict:return nc::Error::Conflict;
 case application::ControlError::malformed:case application::ControlError::invalid_graph:return nc::Error::Validation;
 case application::ControlError::unsupported:return nc::Error::Unsupported;
 case application::ControlError::limit_exceeded:return nc::Error::Limits;
 case application::ControlError::busy:return nc::Error::Busy;
 case application::ControlError::preparation_expired:return nc::Error::Expired;
 case application::ControlError::no_prepared:return nc::Error::Expired;
 case application::ControlError::foreign_preparation:return nc::Error::Foreign;
 default:return nc::Error::Internal;
 }
}
std::optional<std::size_t> control_framed_bound(std::size_t n) noexcept {
 std::size_t value=n;if(!add(value,4))return {};const auto codes=value/223;
 value=n;if(!add(value,6)||!add(value,codes))return {};return value;
}
std::optional<std::size_t> control_message_bound(std::size_t n,std::size_t packet) noexcept {
 if(!n)return {};
 if(n<=packet)return control_framed_bound(n);
 std::size_t result=0;
 const auto chunk=fragment_chunk(packet);
 const auto full=n/chunk,remainder=n%chunk;
 const auto bound=control_framed_bound(chunk+ControlFragmentHeaderBytes);
 if(!bound||!product(result,full,*bound))return {};
 if(remainder){const auto tail=control_framed_bound(remainder+ControlFragmentHeaderBytes);if(!tail||!add(result,*tail))return {};}
 return result;
}
bool control_peer_budget_fits(std::size_t budget,std::size_t device,std::size_t pending,std::size_t assembly,std::size_t outgoing) noexcept {
 for(auto n:{device,pending,assembly,outgoing}){if(n>budget)return false;budget-=n;}return true;
}
bool session_transport_valid(const SessionConfig& config) {
 const auto& limits=config.codec_limits;
 return config.endpoint_authorized && text_valid(config.device_identity) && config.timeout_ms &&
    config.max_packet_bytes>=512 && config.max_packet_bytes<=65535 &&
    config.read_chunk_bytes && config.read_chunk_bytes<=65535 &&
    bool(config.acquire_staging)==bool(config.release_staging) &&
    limits.wire_bytes>=512 && limits.wire_bytes<=4*1024*1024 &&
    limits.arena_bytes && limits.depth && limits.depth<=nc::HardDepth &&
    bool(nc::negotiate(config.schema_version,config.schema_version,config.codec_mask,config.codec_mask));
}
bool session_storage_valid(const SessionConfig& config) {
 auto message=control_message_bound(config.codec_limits.wire_bytes,config.max_packet_bytes);
 const auto packet=control_framed_bound(config.max_packet_bytes);
 if(!message || !packet || !add(*message,*packet) || *message>config.peer_budget_bytes)return false;
 const auto storage=ControlSession::storage_requirements(config);
 return storage && storage->persistent_bytes<=config.storage_budget_bytes;
}
bool frame_profile_fits(const signal_contracts::FrameLimits& frame,const signal_contracts::FrameLimits& transport) {
 return frame.max_planes<=transport.max_planes && frame.max_elements<=transport.max_elements &&
    frame.max_payload_bytes<=transport.max_payload_bytes && frame.max_metadata_bytes<=transport.max_metadata_bytes &&
    frame.max_string_bytes<=transport.max_string_bytes && frame.max_coordinate_bytes<=transport.max_coordinate_bytes;
}
bool admission_profile_fits(const application::NumericProcessingCapabilities& profile,const nc::Limits& limits) {
 const auto& admission=profile.runtime.admission;
 return profile.device.query && admission.max_nodes<=limits.nodes && admission.max_edges<=limits.edges &&
    admission.max_text_bytes<=limits.string_bytes && admission.frame.max_planes<=limits.channels &&
    frame_profile_fits(admission.frame,limits.frame);
}
bool resource_values_equal(const nc::Resources& wire,const domain::ProcessingResources& resources) {
 return wire.plan==resources.plan_bytes && wire.payload==resources.payload_bytes &&
    wire.scratch==resources.scratch_bytes && wire.stack==resources.stack_bytes;
}
bool advertised_resources_fit(const nc::Resources& advertised,const domain::ProcessingResources& resources,
                              const domain::ProcessingResourceProfile& profile) {
 if(!resource_values_equal(advertised,resources) || resources.plan_bytes>profile.maximum.plan_bytes ||
    resources.payload_bytes>profile.maximum.payload_bytes || resources.scratch_bytes>profile.maximum.scratch_bytes ||
    resources.stack_bytes>profile.maximum.stack_bytes || advertised.peak>profile.max_peak_bytes)return false;
 std::size_t reservation=0;
 return add(reservation,resources.plan_bytes) && add(reservation,resources.payload_bytes) &&
    add(reservation,resources.scratch_bytes) && add(reservation,resources.stack_bytes) && reservation<=advertised.peak;
}
bool capability_row_valid(const nc::CapabilityRow& row,const SessionConfig& config) {
 nc::Graph graph;graph.graph_id="capability";graph.nodes=std::span(&row.combination,1);
 domain::NumericProcessingGraph owner;
 if(!own_graph(graph,config.codec_limits,owner))return false;
 const auto& profile=*config.application_capabilities->numeric;
 domain::ResolvedProcessingNode resolved;
 if(!domain::resolve_processing_node(owner.nodes[0],profile.runtime.admission,resolved))return false;
 const auto& maximum=profile.runtime.resources;
 const auto support=profile.device.query(profile.device.context,resolved,maximum);
 return support.supported && advertised_resources_fit(row.resources,support.resources,maximum);
}
bool advertised_profile_valid(const SessionConfig& config,nc::Message& message) {
 nc::Reply reply;reply.capabilities=*config.advertised_capabilities;message.body=reply;
 if(!nc::validate(message,config.codec_limits) || config.advertised_capabilities->codec_mask!=config.codec_mask)return false;
 const auto& maximum=config.application_capabilities->numeric->runtime.resources;
 if(!resource_values_equal(config.advertised_capabilities->maximum,maximum.maximum) ||
    config.advertised_capabilities->maximum.peak!=maximum.max_peak_bytes)return false;
 return std::all_of(config.advertised_capabilities->rows.begin(),config.advertised_capabilities->rows.end(),
    [&](const auto& row){return capability_row_valid(row,config);});
}
bool conservative_reply_fits(const SessionConfig& config,const nc::Result& footprint) {
 if(config.runtime_preflights_reply_envelope)return true;
 const auto& limits=config.codec_limits;
 const auto bytes=config.application_capabilities->numeric->runtime.admission.max_owned_metadata_bytes;
 std::size_t wire=4096,arena=footprint.required_arena,text=footprint.required_wire;
 return add(wire,footprint.required_wire) && product(wire,8,bytes) &&
    product(arena,4,bytes) && add(text,bytes) &&
    wire<=limits.wire_bytes && arena<=limits.arena_bytes && text<=limits.total_text_bytes;
}
bool conservative_records_fit(const SessionConfig& config) {
 if(config.runtime_preflights_reply_envelope)return true;
 const auto bytes=config.application_capabilities->numeric->runtime.admission.max_owned_metadata_bytes;
 const auto& limits=config.codec_limits;
 return bytes<=limits.total_text_bytes && bytes/sizeof(domain::ProcessingPort)<=limits.ports &&
    bytes/sizeof(domain::ProcessingParameter)<=limits.parameters && bytes/4<=limits.records;
}
bool reply_storage_valid(const SessionConfig& config,const nc::Message& message) {
 if(config.acquire_staging){StagingLease lease(config,{});if(!lease.valid)return false;}
 nc::CborCodec cbor;nc::ProtobufCodec protobuf;
 for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}) {
  const auto footprint=codec->measure(message,config.codec_limits);
  if(!footprint || !conservative_reply_fits(config,footprint))return false;
 }
 return conservative_records_fit(config);
}
bool server_configuration_valid(const SessionConfig& config) {
 if(!config.application_capabilities || !config.application_capabilities->numeric ||
    !config.advertised_capabilities || !config.client_identity)return false;
 if(!admission_profile_fits(*config.application_capabilities->numeric,config.codec_limits))return false;
 nc::Message message;
 return advertised_profile_valid(config,message) && reply_storage_valid(config,message);
}
bool configuration_valid(const SessionConfig& config,bool server) {
 if(!session_transport_valid(config) || !session_storage_valid(config))return false;
 return !server || server_configuration_valid(config);
}
}
struct ControlSession::Impl {
 SessionConfig config;
 application::ControlClock& clock;
 application::ProcessingControlService* service;
 ControlSessionState state=ControlSessionState::Disconnected;
 ControlSessionError error=ControlSessionError::None;
 application::SessionIdentity context{};
 std::optional<nc::CodecId> selected;
 nc::CborCodec cbor;nc::ProtobufCodec protobuf;
 // TX remains immutable while flushing. RX is reused only after decoding has
 // copied its views into the disjoint arenas, or holds one deferred request.
 std::vector<std::uint8_t> incoming,outgoing,read,encoded,assembly,fragment;
 std::size_t deferred_bytes=0;
 std::unique_ptr<PacketStream> packets;
 std::vector<std::max_align_t> arena_storage,staging_storage;
 std::span<std::byte> arena,staging;
 ReplyHandler handler=nullptr;void* handler_context=nullptr;
 CommandCommitHandler commit_handler=nullptr;void* commit_context=nullptr;
 bdsp_protocol::PacketHandler packet_handler=nullptr;void* packet_context=nullptr;
 std::size_t effective_bytes=0,effective_packet_bytes=0,device_pending=0,tx_size=0,tx_offset=0,total=0,received=0;
 std::uint8_t tx_kind=0,assembly_kind=0,packet_kind=0;
 std::uint64_t nonce=0,last_nonce=0,tx_sequence=0,assembly_sequence=0,last_assembly_sequence=0;
 std::uint64_t highest_request=0,pending_request=0,handshake_at=0,assembly_at=0,request_at=0;
 nc::CommandId pending_command=nc::CommandId::State;
 bool in_callback=false,reject_after_write=false;
 Impl(SessionConfig c,application::ControlClock& k,application::ProcessingControlService* s)
  :config(std::move(c)),clock(k),service(s) {
  if(!configuration_valid(config,bool(service))){state=ControlSessionState::Failed;error=ControlSessionError::InvalidConfiguration;return;}
  effective_bytes=config.codec_limits.wire_bytes;effective_packet_bytes=config.max_packet_bytes;
  prepare_transport_buffers();prepare_decode_arenas();prepare_packet_stream();
 }
 void prepare_transport_buffers() {
  const auto& l=config.codec_limits;
  incoming.resize(config.max_packet_bytes+4);outgoing.resize(*control_framed_bound(config.max_packet_bytes));read.resize(config.read_chunk_bytes);
  encoded.resize(l.wire_bytes);assembly.resize(l.wire_bytes);
  fragment.resize(fragment_chunk(config.max_packet_bytes)+ControlFragmentHeaderBytes);
 }
 void prepare_decode_arenas() {
  const auto& l=config.codec_limits;
  const auto words=(l.arena_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t);
  arena_storage.resize(words); staging_storage.resize(config.acquire_staging?0:words);
  arena={reinterpret_cast<std::byte*>(arena_storage.data()),l.arena_bytes};
  staging={reinterpret_cast<std::byte*>(staging_storage.data()),staging_storage.size()*sizeof(std::max_align_t)};
 }
 void prepare_packet_stream() {
  packets=std::make_unique<PacketStream>(incoming,outgoing);
  packets->set_packet_handler([](PacketView view,void* self){static_cast<Impl*>(self)->packet(view);},this);
  packets->set_error_handler([](auto,void* self){static_cast<Impl*>(self)->fail(ControlSessionError::Malformed);},this);
 }
 ~Impl(){close_context();}
 void close_context() noexcept {if(service && context.session)service->close_session(context);context={};}
 void clear() noexcept {
  close_context();if(packets)packets->reset();selected.reset();tx_size=tx_offset=total=received=0;
  deferred_bytes=0;
  packet_kind=0;
  assembly_sequence=last_assembly_sequence=tx_sequence=highest_request=pending_request=0;
  // Nonces identify a physical negotiation, not a globally ordered client.
  // reset closes the issued application session; its new session ID fences old
  // commands/DATA even when the next client's random nonce is lower or equal.
  nonce=last_nonce=0;
  handshake_at=assembly_at=request_at=0;reject_after_write=false;effective_bytes=config.codec_limits.wire_bytes;effective_packet_bytes=config.max_packet_bytes;
 }
 void fail(ControlSessionError reason) noexcept {clear();state=ControlSessionState::Failed;error=reason;}
 const nc::Codec& codec() const {return *selected==nc::CodecId::Cbor?static_cast<const nc::Codec&>(cbor):static_cast<const nc::Codec&>(protobuf);}
 nc::Limits limits() const {auto l=config.codec_limits;l.wire_bytes=effective_bytes;return l;}
 bool budget(std::size_t reservation) const {
  return control_peer_budget_fits(config.peer_budget_bytes,device_pending,packets->pending_bytes(),total+deferred_bytes,reservation);
 }
 bool queue(std::uint8_t id,std::span<const std::uint8_t> payload) {
  if(tx_size||packets->pending_bytes()||payload.empty()||payload.size()>effective_bytes)return false;
  if(!reserve_transmission(payload.size()))return false;
  if(payload.data()!=encoded.data())std::memcpy(encoded.data(),payload.data(),payload.size());
  tx_kind=id;tx_size=payload.size();tx_offset=0;return true;
 }
 bool reserve_transmission(std::size_t size) {
  auto bound=control_message_bound(size,effective_packet_bytes);const auto packet=control_framed_bound(effective_packet_bytes);
  if(!bound||!packet||!add(*bound,*packet)||!budget(*bound))return false;
  if(size>effective_packet_bytes){if(tx_sequence==UINT64_MAX)return false;++tx_sequence;}
  return true;
 }
 bool encode_message(const nc::Message& message,std::uint8_t id) {
  // All decoded views live in arena, not assembly. Encoding cannot overlap an
  // incomplete or deferred inbound message; destination stays disjoint.
  if(total||deferred_bytes)return false;
  const auto result=codec().encode(message,encoded,assembly,limits());
  return result && queue(id,std::span<const std::uint8_t>(encoded).first(result.produced));
 }
 bool emit() {
  if(packets->pending_bytes()||!tx_size)return true;
  if(tx_offset==tx_size){tx_size=tx_offset=0;return true;}
  if(tx_size<=effective_packet_bytes)return emit_packet();
  return emit_fragment();
 }
 bool emit_packet() {
   if(packets->enqueue(tx_kind,std::span<const std::uint8_t>(encoded).first(tx_size))!=Status::ok)return false;
   packet_kind=tx_kind;
   tx_offset=tx_size;return true;
 }
 bool emit_fragment() {
  const auto count=std::min(fragment_chunk(effective_packet_bytes),tx_size-tx_offset);std::size_t at=0;
  put(fragment,at,1,2);put(fragment,at,context.session,8);put(fragment,at,tx_sequence,8);
  put(fragment,at,tx_size,4);put(fragment,at,tx_offset,4);put(fragment,at,tx_kind,1);put(fragment,at,count,2);
  std::memcpy(fragment.data()+at,encoded.data()+tx_offset,count);
  if(packets->enqueue(ControlFragmentId,std::span<const std::uint8_t>(fragment).first(at+count))!=Status::ok)return false;
  packet_kind=ControlFragmentId;
  tx_offset+=count;return true;
 }
 std::size_t encode_ack(std::uint8_t status,std::uint64_t request_nonce) {
  std::size_t at=0;
  put(assembly,at,HelloVersion,2);put(assembly,at,request_nonce,8);put(assembly,at,status,1);
  put(assembly,at,status?0:config.schema_version,4);put(assembly,at,status?0:(*selected==nc::CodecId::Cbor?1:2),1);
  put(assembly,at,status?0:effective_bytes,4);put(assembly,at,status?0:effective_packet_bytes,4);put(assembly,at,status?0:config.allow_mutations,1);
  put(assembly,at,status?0:context.client,8);put(assembly,at,status?0:context.session,8);put(assembly,at,status?0:context.epoch,8);
  put_text(assembly,at,config.device_identity);put_text(assembly,at,{});return at;
 }
 void ack(std::uint8_t status,ControlSessionError reason,std::uint64_t request_nonce) {
  // Rejection uses the local fixed envelope before negotiated limits exist.
  if(status)effective_bytes=config.codec_limits.wire_bytes;
  const auto count=encode_ack(status,request_nonce);
  if(!queue(ControlHelloAckId,std::span<const std::uint8_t>(assembly).first(count))){fail(ControlSessionError::LimitExceeded);return;}
  if(status){error=reason;state=ControlSessionState::Negotiating;handshake_at=clock.now_ms();reject_after_write=true;}
  else state=ControlSessionState::Ready;
 }
 struct HelloRequest {
  Reader reader;
  std::uint64_t version,nonce,schema,mask,maximum,packet_maximum;
  std::string_view device;
  explicit HelloRequest(std::span<const std::uint8_t> bytes):reader{bytes} {
   version=reader.number(2);nonce=reader.number(8);schema=reader.number(4);mask=reader.number(4);
   maximum=reader.number(4);packet_maximum=reader.number(4);device=reader.text(256);
  }
 };
 bool hello_request_valid(const HelloRequest& request) const {
  return request.reader.bytes.size()<=512 && request.reader.end() && request.version==HelloVersion &&
    request.packet_maximum>=512 && request.packet_maximum<=65535 && request.nonce &&
    request.nonce>last_nonce && state==ControlSessionState::Disconnected;
 }
 bool negotiate_hello(const HelloRequest& request) {
  if(request.schema!=config.schema_version){ack(2,ControlSessionError::Unsupported,request.nonce);return false;}
  if(request.device!=config.device_identity){ack(8,ControlSessionError::WrongDevice,request.nonce);return false;}
  selected=nc::negotiate(config.schema_version,std::uint32_t(request.schema),config.codec_mask,std::uint32_t(request.mask));
  if(!selected){ack(3,ControlSessionError::Unsupported,request.nonce);return false;}
  effective_bytes=std::min(config.codec_limits.wire_bytes,std::size_t(request.maximum));
  if(effective_bytes<config.codec_limits.wire_bytes){ack(4,ControlSessionError::LimitExceeded,request.nonce);return false;}
  return true;
 }
 void hello(std::span<const std::uint8_t> bytes) {
  const HelloRequest request(bytes);
  if(!hello_request_valid(request)){fail(ControlSessionError::Malformed);return;}
  last_nonce=request.nonce;effective_packet_bytes=std::min(config.max_packet_bytes,std::size_t(request.packet_maximum));
  if(!negotiate_hello(request))return;
  const auto opened=service->open_session(config.client_identity);
  if(!opened){ack(opened.error==application::ControlError::busy?6:4,ControlSessionError::Busy,request.nonce);return;}
  context=opened.identity;nonce=request.nonce;ack(0,ControlSessionError::None,request.nonce);
 }
 struct HelloAcknowledgement {
  Reader reader;
  std::uint64_t version,nonce,status,schema,id,maximum,packet_maximum,writable;
  application::SessionIdentity issued;
  std::string_view device;
  explicit HelloAcknowledgement(std::span<const std::uint8_t> bytes):reader{bytes} {
   version=reader.number(2);nonce=reader.number(8);status=reader.number(1);schema=reader.number(4);
   id=reader.number(1);maximum=reader.number(4);packet_maximum=reader.number(4);writable=reader.number(1);
   issued={reader.number(8),reader.number(8),reader.number(8)};device=reader.text(256);
   reader.text(256,true);
  }
  bool canonical_rejection() const {
   return !schema && !id && !maximum && !packet_maximum && !writable && !issued.client && !issued.session && !issued.epoch;
  }
 };
 static ControlSessionError acknowledgement_error(std::uint64_t status) {
  switch(status) {
  case 8:return ControlSessionError::WrongDevice;
  case 4:return ControlSessionError::LimitExceeded;
  case 6:return ControlSessionError::Busy;
  default:return ControlSessionError::Unsupported;
  }
 }
 bool acknowledgement_valid(const HelloAcknowledgement& reply) const {
  return reply.reader.bytes.size()<=1024 && reply.reader.end() && reply.version==HelloVersion &&
    reply.nonce==nonce && state==ControlSessionState::Negotiating && reply.status<=8 && reply.writable<=1;
 }
 bool accepted_limits_valid(const HelloAcknowledgement& reply) const {
  return reply.maximum && reply.maximum<=config.codec_limits.wire_bytes &&
    reply.packet_maximum>=512 && reply.packet_maximum<=config.max_packet_bytes &&
    reply.issued.client && reply.issued.session && reply.issued.epoch;
 }
 void accept_acknowledgement(const HelloAcknowledgement& reply) {
  if(reply.device!=config.device_identity){fail(ControlSessionError::WrongDevice);return;}
  const auto mask=reply.id==1?nc::CborMask:reply.id==2?nc::ProtobufMask:0;
  const auto chosen=nc::negotiate(config.schema_version,std::uint32_t(reply.schema),config.codec_mask,mask);
  if(!chosen || !accepted_limits_valid(reply)){fail(ControlSessionError::Malformed);return;}
  selected=chosen;context=reply.issued;effective_bytes=std::size_t(reply.maximum);
  effective_packet_bytes=std::size_t(reply.packet_maximum);config.allow_mutations=bool(reply.writable);state=ControlSessionState::Ready;
 }
 void hello_ack(std::span<const std::uint8_t> bytes) {
  const HelloAcknowledgement reply(bytes);
  if(!acknowledgement_valid(reply)){fail(ControlSessionError::Malformed);return;}
  if(reply.status) {
   fail(reply.canonical_rejection()?acknowledgement_error(reply.status):ControlSessionError::Malformed);return;
  }
  accept_acknowledgement(reply);
 }
 bool defer_message(std::span<const std::uint8_t> bytes) {
  if(deferred_bytes || bytes.size()>assembly.size()){fail(ControlSessionError::Busy);return false;}
  if(bytes.data()!=assembly.data())std::memcpy(assembly.data(),bytes.data(),bytes.size());
  deferred_bytes=bytes.size();return true;
 }
 nc::Result decode_message(std::span<const std::uint8_t> bytes,nc::Message& wire) {
  StagingLease lease(config,staging);
  if(!lease.valid)return {nc::Status::BufferTooSmall};
  if(overlaps(lease.bytes,arena) || overlaps(lease.bytes,std::as_bytes(bytes)) ||
     overlaps(lease.bytes,std::as_bytes(std::span(encoded))) || overlaps(lease.bytes,std::as_bytes(std::span(assembly))))
   return {nc::Status::InvalidArgument};
  return codec().decode(bytes,arena,lease.bytes,wire,limits());
 }
 void notify_reply(const nc::Message& wire) {
  if(!handler)return;
  in_callback=true;handler(wire,handler_context);in_callback=false;
 }
 void notify_commit(const application::ProcessingReply& reply) {
  if(!commit_handler)return;
  in_callback=true;commit_handler(reply,commit_context);in_callback=false;
 }
 bool command_authorized(const nc::Message& wire,const nc::Command* command) {
  if(!command || !wire.request){fail(ControlSessionError::Malformed);return false;}
  if(wire.request<highest_request){fail(ControlSessionError::Replay);return false;}
  if(!config.allow_mutations && command->id!=nc::CommandId::State && command->id!=nc::CommandId::Capabilities) {
   fail(ControlSessionError::ReadOnly);return false;
  }
  return true;
 }
 bool map_parameters(const nc::Command& command,application::ProcessingCommand& mapped) {
  application::SetNumericProcessingParameters parameters;parameters.node=command.node;parameters.enabled=command.enabled;
  if(!own_parameters(*command.parameters,limits(),parameters.parameters))return false;
  mapped=std::move(parameters);return true;
 }
 bool map_prepare(const nc::Command& command,application::ProcessingDTO& dto,application::ProcessingCommand& mapped) {
  domain::NumericProcessingGraph graph;
  if(!own_graph(*command.graph,limits(),graph))return false;
  dto.numeric=std::move(graph);mapped=application::PrepareProcessing{&dto};return true;
 }
 bool map_command(const nc::Command& command,application::ProcessingDTO& dto,application::ProcessingCommand& mapped) {
  switch(command.id) {
  case nc::CommandId::Capabilities:mapped=application::GetCapabilities{};return true;
  case nc::CommandId::State:mapped=application::GetState{};return true;
  case nc::CommandId::Prepare:return map_prepare(command,dto,mapped);
  case nc::CommandId::Apply:mapped=application::ApplyProcessing{command.token,command.graph_id};return true;
  case nc::CommandId::Release:mapped=application::ReleasePrepared{command.token};return true;
  case nc::CommandId::SetParameters:return map_parameters(command,mapped);
  case nc::CommandId::Persist:mapped=application::ExplicitPersist{command.graph_revision};return true;
  default:return false;
  }
 }
 bool reply_budget_available() const {
  const auto bound=control_message_bound(effective_bytes,effective_packet_bytes);
  const auto packet=control_framed_bound(effective_packet_bytes);
  std::size_t reservation=bound?*bound:0;
  return bound && packet && add(reservation,*packet) && budget(reservation);
 }
 static bool diagnostic_has_port(domain::ProcessingError error) {
  switch(error) {
  case domain::ProcessingError::InvalidPort:case domain::ProcessingError::InvalidContract:
  case domain::ProcessingError::IncompatiblePorts:case domain::ProcessingError::PreparationValueUnavailable:
  case domain::ProcessingError::PreparationTypeMismatch:case domain::ProcessingError::PreparationValueInvalid:
  case domain::ProcessingError::PreparationValueOutOfRange:return true;
  default:return false;
  }
 }
 static nc::Diagnostic wire_diagnostic(const domain::ProcessingDiagnostic& validation) {
  nc::Diagnostic diagnostic;
  if(validation)return diagnostic;
  if(validation.node>=0)diagnostic.node=validation.node;
  if(diagnostic_has_port(validation.error))diagnostic.port=validation.port;
  diagnostic.edge=validation.edge;diagnostic.parameter=validation.parameter;diagnostic.reason=validation.reason;return diagnostic;
 }
 static nc::Lifecycle wire_lifecycle(application::LifecycleState lifecycle) {
  switch(lifecycle) {
  case application::LifecycleState::Empty:return nc::Lifecycle::Empty;
  case application::LifecycleState::Ready:return nc::Lifecycle::Prepared;
  case application::LifecycleState::Running:return nc::Lifecycle::Running;
  case application::LifecycleState::Stopped:return nc::Lifecycle::Stopped;
  }
  return nc::Lifecycle::Empty;
 }
 static nc::Reply wire_reply(const application::ProcessingReply& reply,nc::CommandId command) {
  nc::Reply result;result.command=command;result.error=reply_error(reply.error);
  result.state.revision=reply.state.revision;result.state.run_intent=reply.state.run_requested;
  result.state.token=reply.prepared_token?reply.prepared_token:reply.state.own_prepared_token;
  result.state.lifecycle=wire_lifecycle(reply.state.lifecycle);
  result.diagnostic=wire_diagnostic(reply.numeric_validation);
  if(reply.numeric_resources) {
   const auto& admission=*reply.numeric_resources;const auto& resources=admission.resources;
   result.state.resources={resources.plan_bytes,resources.payload_bytes,resources.scratch_bytes,resources.stack_bytes,admission.peak_bytes};
  }
  return result;
 }
 nc::Message response_envelope(const nc::Message& request,const nc::Reply& reply) const {
  nc::Message out;out.client=context.client;out.session=context.session;out.epoch=context.epoch;
  out.request=request.request;out.expected_revision=request.expected_revision;out.body=reply;return out;
 }
 void encode_reply(const nc::Message& request,const nc::Reply& reply,const application::ProcessingReply& source) {
  auto out=response_envelope(request,reply);
  // Graph and persisted revisions retain their exact values in the bounded extension.
  std::array<std::uint8_t,16> revisions{};std::size_t at=0;
  put(revisions,at,source.state.graph_revision,8);put(revisions,at,source.state.persisted_graph_revision,8);
  nc::Extension extension{1024,revisions};out.extensions=std::span(&extension,1);
  if(!encode_message(out,ControlReplyId)){fail(ControlSessionError::Codec);return;}
  notify_reply(out);
 }
 void send_reply(const nc::Message& request,const application::ProcessingReply& source,nc::CommandId command) {
  auto reply=wire_reply(source,command);GraphProjection graph;
  if(source.state.applied_numeric) {
   if(!graph.assign(*source.state.applied_numeric,limits())){fail(ControlSessionError::Mapping);return;}
   reply.state.applied=graph.wire();
  }
  if(command==nc::CommandId::Capabilities)reply.capabilities=*config.advertised_capabilities;
  encode_reply(request,reply,source);
 }
 void server_message(const nc::Message& wire) {
  const auto* command=std::get_if<nc::Command>(&wire.body);
  if(!command_authorized(wire,command))return;
  application::ProcessingDTO dto;application::ProcessingCommand mapped;
  if(!map_command(*command,dto,mapped)){fail(ControlSessionError::Mapping);return;}
  if(!reply_budget_available()){fail(ControlSessionError::LimitExceeded);return;}
  highest_request=std::max(highest_request,wire.request);
  const auto reply=service->submit(context,{wire.request,wire.expected_revision},mapped);
  notify_commit(reply);send_reply(wire,reply,command->id);
 }
 void client_message(const nc::Message& wire) {
  const auto* reply=std::get_if<nc::Reply>(&wire.body);
  if(!reply || !pending_request || wire.request!=pending_request || reply->command!=pending_command) {
   fail(ControlSessionError::Identity);return;
  }
  pending_request=0;notify_reply(wire);
 }
 void message(std::uint8_t id,std::span<const std::uint8_t> bytes) {
  if(state!=ControlSessionState::Ready || !selected || id!=(service?ControlCommandId:ControlReplyId)) {
   fail(ControlSessionError::Unsupported);return;
  }
  if(service && (tx_size || packets->pending_bytes())){defer_message(bytes);return;}
  nc::Message wire;
  if(!decode_message(bytes,wire)){fail(ControlSessionError::Codec);return;}
  // The decode lease has ended; only primary-arena views reach application callbacks.
  if(application::SessionIdentity{wire.client,wire.session,wire.epoch}!=context){fail(ControlSessionError::Identity);return;}
  dispatch_message(wire);
 }
 void dispatch_message(const nc::Message& wire) {
  if(service)server_message(wire);
  else client_message(wire);
 }

 Status outgoing_command_status(const nc::Command& command,std::uint64_t request) {
  if(in_callback || service || state!=ControlSessionState::Ready)return Status::disconnected;
  if(pending_request || tx_size || packets->pending_bytes() || total || deferred_bytes)return Status::would_block;
  if(!request || request<highest_request)return Status::invalid_argument;
  if(command.graph && nc::graph_versions_supported(command.graph->versions)!=Status::ok)
   return Status::unsupported_version;
  if(!config.allow_mutations && command.id!=nc::CommandId::State && command.id!=nc::CommandId::Capabilities) {
   error=ControlSessionError::ReadOnly;return Status::unavailable;
  }
  return Status::ok;
 }
 nc::Message command_envelope(const nc::Command& command,std::uint64_t request,std::uint64_t expected_revision) const {
  nc::Message out;out.client=context.client;out.session=context.session;out.epoch=context.epoch;
  out.request=request;out.expected_revision=expected_revision;out.body=command;return out;
 }
 Status send_command(const nc::Command& command,std::uint64_t request,std::uint64_t expected_revision) {
  const auto status=outgoing_command_status(command,request);if(status!=Status::ok)return status;
  const auto out=command_envelope(command,request,expected_revision);
  if(!encode_message(out,ControlCommandId))return Status::exhausted;
  highest_request=std::max(highest_request,request);pending_request=request;pending_command=command.id;
  request_at=clock.now_ms();return Status::ok;
 }
 bool expired(std::uint64_t at,std::uint64_t now) const {
  return now<at || now-at>=config.timeout_ms;
 }
 bool session_expired() const {
  const auto now=clock.now_ms();
  return (state==ControlSessionState::Negotiating && expired(handshake_at,now)) ||
    (total && expired(assembly_at,now)) || (pending_request && expired(request_at,now));
 }
 Status poll_preflight(ByteTransport& transport,std::int64_t pending) {
  if(in_callback)return Status::invalid_argument;
  if(!packets || state==ControlSessionState::Failed)return Status::disconnected;
  if(transport.state()!=TransportState::connected || pending<0){fail(ControlSessionError::Disconnected);return Status::disconnected;}
  device_pending=std::size_t(pending);
  if(session_expired()){fail(ControlSessionError::Timeout);return Status::unavailable;}
  // Service submit owns preparation expiry and preserves rejection provenance.
  return validate_outbound_budget();
 }
 Status validate_outbound_budget() {
  const auto reservation=tx_size?control_message_bound(tx_size,effective_packet_bytes):std::optional<std::size_t>(0);
  if(!reservation || !budget(*reservation)){fail(ControlSessionError::LimitExceeded);return Status::exhausted;}
  return Status::ok;
 }
 Status read_transport(ByteTransport& transport) {
  const auto input=transport.read(read);
  if(input.transferred>read.size()){fail(ControlSessionError::Transport);return Status::invalid_argument;}
  if(input.transferred)packets->feed(std::span<const std::uint8_t>(read).first(input.transferred));
  if(input.status!=Status::ok && input.status!=Status::would_block){fail(ControlSessionError::Transport);return input.status;}
  return state==ControlSessionState::Failed?Status::malformed:Status::ok;
 }
 void retire_transmission() {
  if(packets->pending_bytes())return;
  packet_kind=0;
  if(!tx_size || tx_offset!=tx_size)return;
  tx_size=tx_offset=0;
  if(reject_after_write){const auto reason=error;fail(reason);}
 }
 Status flush_transport(ByteTransport& transport) {
  if(!emit()){fail(ControlSessionError::Transport);return Status::exhausted;}
  const auto output=packets->flush(transport);
  if(output.status!=Status::ok && output.status!=Status::would_block){fail(ControlSessionError::Transport);return output.status;}
  retire_transmission();return Status::ok;
 }
 void dispatch_deferred() {
  if(state!=ControlSessionState::Ready || !deferred_bytes || tx_size || packets->pending_bytes())return;
  const auto count=deferred_bytes;deferred_bytes=0;
  message(ControlCommandId,std::span<const std::uint8_t>(assembly).first(count));
 }
 Status poll_transport(ByteTransport& transport,std::int64_t pending) {
  auto status=poll_preflight(transport,pending);if(status!=Status::ok)return status;
  status=read_transport(transport);if(status!=Status::ok)return status;
  status=flush_transport(transport);if(status!=Status::ok)return status;
  dispatch_deferred();return Status::ok;
 }
 struct FragmentView {
  Reader reader;
  std::uint64_t version,session,sequence,length,offset,kind,count;
  explicit FragmentView(std::span<const std::uint8_t> bytes):reader{bytes} {
   version=reader.number(2);session=reader.number(8);sequence=reader.number(8);
   length=reader.number(4);offset=reader.number(4);kind=reader.number(1);count=reader.number(2);
  }
 };
 bool fragment_valid(const FragmentView& fragment) const {
  return fragment.reader.valid && fragment.version==1 && fragment.session==context.session &&
    fragment.sequence && fragment.count && fragment.count<=fragment_chunk(effective_packet_bytes) &&
    fragment.count==fragment.reader.bytes.size()-fragment.reader.at &&
    fragment.length>effective_packet_bytes && fragment.length<=effective_bytes &&
    fragment.offset<=fragment.length && fragment.count<=fragment.length-fragment.offset &&
    fragment.kind==(service?ControlCommandId:ControlReplyId);
 }
 bool start_assembly(const FragmentView& fragment) {
  if(deferred_bytes){fail(ControlSessionError::Busy);return false;}
  if(fragment.offset || fragment.sequence<=last_assembly_sequence){fail(ControlSessionError::Replay);return false;}
  const auto outgoing=tx_size?*control_message_bound(tx_size,effective_packet_bytes):0;
  if(!control_peer_budget_fits(config.peer_budget_bytes,device_pending,packets->pending_bytes(),std::size_t(fragment.length),outgoing)) {
   fail(ControlSessionError::LimitExceeded);return false;
  }
  total=std::size_t(fragment.length);received=0;assembly_sequence=fragment.sequence;
  assembly_kind=std::uint8_t(fragment.kind);assembly_at=clock.now_ms();return true;
 }
 bool fragment_continues_assembly(const FragmentView& fragment) const {
  const auto now=clock.now_ms();
  return fragment.sequence==assembly_sequence && fragment.kind==assembly_kind &&
    fragment.length==total && fragment.offset==received && now>=assembly_at && now-assembly_at<config.timeout_ms;
 }
 void finish_assembly() {
  const auto complete=total;const auto completed_kind=assembly_kind;
  last_assembly_sequence=assembly_sequence;total=received=0;assembly_sequence=assembly_at=0;
  message(completed_kind,std::span<const std::uint8_t>(assembly).first(complete));
 }
 void fragment_packet(std::span<const std::uint8_t> payload) {
  if(state!=ControlSessionState::Ready){fail(ControlSessionError::Unsupported);return;}
  const FragmentView fragment(payload);
  if(!fragment_valid(fragment)){fail(ControlSessionError::Malformed);return;}
  if(!total && !start_assembly(fragment))return;
  if(!fragment_continues_assembly(fragment)){fail(ControlSessionError::Malformed);return;}
  append_fragment(payload,fragment);
 }
 void append_fragment(std::span<const std::uint8_t> payload,const FragmentView& fragment) {
  std::memcpy(assembly.data()+received,payload.data()+fragment.reader.at,std::size_t(fragment.count));
  received+=std::size_t(fragment.count);
  if(received==total)finish_assembly();
 }
 bool notify_packet(PacketView view) {
  if(view.id==ControlCommandId || view.id==ControlReplyId || view.id==ControlFragmentId ||
     state!=ControlSessionState::Ready || !packet_handler)return false;
  in_callback=true;packet_handler(view,packet_context);in_callback=false;return true;
 }
 void dispatch_packet(PacketView view) {
  if(view.id==ControlHelloId && service){hello(view.payload);return;}
  if(view.id==ControlHelloAckId && !service){hello_ack(view.payload);return;}
  if(notify_packet(view))return;
  if(view.id==ControlFragmentId)fragment_packet(view.payload);
  else message(view.id,view.payload);
 }
 void packet(PacketView view) {
  if(state==ControlSessionState::Failed)return;
  if(view.payload.size()>effective_packet_bytes){fail(ControlSessionError::LimitExceeded);return;}
  if(total && view.id!=ControlFragmentId){fail(ControlSessionError::Malformed);return;}
  dispatch_packet(view);
 }
};
ControlSession::ControlSession(SessionConfig config,application::ControlClock& clock,application::ProcessingControlService* service)
 :impl_(std::make_unique<Impl>(std::move(config),clock,service)){}
ControlSession::~ControlSession()=default;
std::optional<SessionStorageRequirements> ControlSession::storage_requirements(const SessionConfig& c) noexcept {
 if(c.max_packet_bytes<512||c.max_packet_bytes>65535||!c.read_chunk_bytes||c.read_chunk_bytes>65535||
    bool(c.acquire_staging)!=bool(c.release_staging))return {};
 const auto packet=control_framed_bound(c.max_packet_bytes);if(!packet)return {};
 const auto& l=c.codec_limits;
 std::size_t rounded=l.arena_bytes;
 if(!add(rounded,sizeof(std::max_align_t)-1))return {};
 rounded=(rounded/sizeof(std::max_align_t))*sizeof(std::max_align_t);
 SessionStorageRequirements r;r.persistent_bytes=sizeof(Impl)+sizeof(PacketStream);
 if(!add(r.persistent_bytes,c.device_identity.capacity())||!add(r.persistent_bytes,1)||
    !product(r.persistent_bytes,2,l.wire_bytes)||!product(r.persistent_bytes,c.acquire_staging?1:2,rounded)||
    !add(r.persistent_bytes,c.max_packet_bytes+4)||!add(r.persistent_bytes,*packet)||
    !add(r.persistent_bytes,c.read_chunk_bytes)||!add(r.persistent_bytes,fragment_chunk(c.max_packet_bytes)+ControlFragmentHeaderBytes))return {};
 r.external_staging_bytes=c.acquire_staging?l.arena_bytes:0;
 return r;
}
Status ControlSession::open(std::uint64_t nonce) {
 auto& d=*impl_;if(d.in_callback||d.service||!d.packets||d.state!=ControlSessionState::Disconnected||!nonce||nonce<=d.last_nonce)return Status::invalid_argument;
 d.nonce=d.last_nonce=nonce;std::size_t at=0;
 put(d.assembly,at,HelloVersion,2);put(d.assembly,at,nonce,8);put(d.assembly,at,d.config.schema_version,4);put(d.assembly,at,d.config.codec_mask,4);
 put(d.assembly,at,d.effective_bytes,4);put(d.assembly,at,d.config.max_packet_bytes,4);put_text(d.assembly,at,d.config.device_identity);
 if(!d.queue(ControlHelloId,std::span<const std::uint8_t>(d.assembly).first(at)))return Status::exhausted;
 d.state=ControlSessionState::Negotiating;d.handshake_at=d.clock.now_ms();return Status::ok;
}
Status ControlSession::send(const nc::Command& command,std::uint64_t request,std::uint64_t expected_revision) {
 return impl_->send_command(command,request,expected_revision);
}
Status ControlSession::poll(ByteTransport& transport,std::int64_t device_pending) {
 return impl_->poll_transport(transport,device_pending);
}
void ControlSession::reset() noexcept {auto& d=*impl_;if(d.in_callback)return;d.clear();if(d.packets){d.state=ControlSessionState::Disconnected;d.error=ControlSessionError::None;}}
void ControlSession::set_command_commit_handler(CommandCommitHandler handler,void* context) noexcept {if(!impl_->in_callback){impl_->commit_handler=handler;impl_->commit_context=context;}}
void ControlSession::set_reply_handler(ReplyHandler handler,void* context) noexcept {if(!impl_->in_callback){impl_->handler=handler;impl_->handler_context=context;}}
void ControlSession::set_packet_handler(bdsp_protocol::PacketHandler handler,void* context) noexcept {if(!impl_->in_callback){impl_->packet_handler=handler;impl_->packet_context=context;}}
Status ControlSession::send_packet(std::uint8_t id,std::span<const std::uint8_t> payload) {
 auto& d=*impl_;
 if(d.in_callback||d.state!=ControlSessionState::Ready)return Status::disconnected;
 if(id==ControlHelloId||id==ControlHelloAckId||id==ControlCommandId||id==ControlReplyId||id==ControlFragmentId||payload.size()>d.effective_packet_bytes)return Status::invalid_argument;
 if(d.tx_size||d.packets->pending_bytes()||d.deferred_bytes)return Status::would_block;
 return d.queue(id,payload)?Status::ok:Status::exhausted;
}
Status ControlSession::discard_outbound_data() noexcept {
 auto& d=*impl_;
 if(d.in_callback)return Status::invalid_argument;
 if(!d.packets||d.state!=ControlSessionState::Ready)return Status::disconnected;
 if(data_packet(d.tx_kind)){d.tx_size=d.tx_offset=0;d.tx_kind=0;}
 if(data_packet(d.packet_kind)&&d.packets->pending_bytes()) {
  if(!d.packets->discard_unstarted_outgoing())return Status::would_block;
  d.packet_kind=0;
 }
 return Status::ok;
}
Status ControlSession::update_capabilities(const nc::Capabilities* capabilities) {
 auto& d=*impl_;if(d.in_callback||!d.service||!capabilities)return Status::invalid_argument;
 // Compact admission certifies graph + the SAME immutable capability snapshot.
 // Replacing it without re-admitting all pending/working graphs breaks that proof.
 if(d.config.runtime_preflights_reply_envelope)return Status::unavailable;
 auto candidate=d.config;candidate.advertised_capabilities=capabilities;
 if(!configuration_valid(candidate,true))return Status::invalid_argument;
 d.config.advertised_capabilities=capabilities;return Status::ok;
}
ControlSessionState ControlSession::state() const noexcept{return impl_->state;}
ControlSessionError ControlSession::error() const noexcept{return impl_->error;}
application::SessionIdentity ControlSession::identity() const noexcept{return impl_->context;}
std::uint64_t ControlSession::nonce() const noexcept{return impl_->nonce;}
std::size_t ControlSession::packet_payload_capacity() const noexcept{return impl_->effective_packet_bytes;}
std::optional<nc::CodecId> ControlSession::codec() const noexcept{return impl_->selected;}
std::size_t ControlSession::pending_bytes() const noexcept{return impl_->tx_size+ (impl_->packets?impl_->packets->pending_bytes():0);}
std::size_t ControlSession::assembly_bytes() const noexcept{return impl_->total;}
}
