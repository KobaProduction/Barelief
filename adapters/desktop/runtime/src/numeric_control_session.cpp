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
bool configuration_valid(const SessionConfig& c,bool server) {
 const auto& l=c.codec_limits;
 if(!c.endpoint_authorized||!text_valid(c.device_identity)||!c.timeout_ms||c.max_packet_bytes<512||c.max_packet_bytes>65535||
    !c.read_chunk_bytes||c.read_chunk_bytes>65535||bool(c.acquire_staging)!=bool(c.release_staging)||
    l.wire_bytes<512||l.wire_bytes>2*1024*1024||!l.arena_bytes||!l.depth||l.depth>nc::HardDepth||
    !nc::negotiate(c.schema_version,c.schema_version,c.codec_mask,c.codec_mask))return false;
 auto message=control_message_bound(l.wire_bytes,c.max_packet_bytes);
 const auto packet=control_framed_bound(c.max_packet_bytes);
 if(!message||!packet||!add(*message,*packet)||*message>c.peer_budget_bytes)return false;
 const auto storage=ControlSession::storage_requirements(c);
 if(!storage||storage->persistent_bytes>c.storage_budget_bytes)return false;
 if(server) {
  if(!c.application_capabilities||!c.application_capabilities->numeric||!c.advertised_capabilities||!c.client_identity)return false;
  const auto& profile=*c.application_capabilities->numeric;
  const auto& admission=profile.runtime.admission;
  if(!profile.device.query||admission.max_nodes>l.nodes||admission.max_edges>l.edges||
     admission.max_text_bytes>l.string_bytes||admission.frame.max_planes>l.channels)return false;
  const auto& frame=admission.frame;const auto& transport_frame=l.frame;
  if(frame.max_planes>transport_frame.max_planes||frame.max_elements>transport_frame.max_elements||
     frame.max_payload_bytes>transport_frame.max_payload_bytes||frame.max_metadata_bytes>transport_frame.max_metadata_bytes||
     frame.max_string_bytes>transport_frame.max_string_bytes||frame.max_coordinate_bytes>transport_frame.max_coordinate_bytes)return false;
  nc::Message m; nc::Reply r; r.capabilities=*c.advertised_capabilities; m.body=r;
  if(!nc::validate(m,l)||c.advertised_capabilities->codec_mask!=c.codec_mask)return false;
  const auto& maximum=profile.runtime.resources;
  const auto& advertised=c.advertised_capabilities->maximum;
  if(advertised.plan!=maximum.maximum.plan_bytes||advertised.payload!=maximum.maximum.payload_bytes||
     advertised.scratch!=maximum.maximum.scratch_bytes||advertised.stack!=maximum.maximum.stack_bytes||
     advertised.peak!=maximum.max_peak_bytes)return false;
  for(const auto& row:c.advertised_capabilities->rows) {
   nc::Graph graph;graph.graph_id="capability";graph.nodes=std::span(&row.combination,1);
   domain::NumericProcessingGraph owner;
   if(!own_graph(graph,l,owner))return false;
   domain::ResolvedProcessingNode resolved;
   if(!domain::resolve_processing_node(owner.nodes[0],admission,resolved))return false;
   auto device=profile.device;device.profile=maximum;
   const auto support=device.query(device.context,resolved,maximum);
   if(!support.supported||row.resources.plan!=support.resources.plan_bytes||
      row.resources.payload!=support.resources.payload_bytes||row.resources.scratch!=support.resources.scratch_bytes||
      row.resources.stack!=support.resources.stack_bytes)return false;
   if(support.resources.plan_bytes>maximum.maximum.plan_bytes||support.resources.payload_bytes>maximum.maximum.payload_bytes||
      support.resources.scratch_bytes>maximum.maximum.scratch_bytes||support.resources.stack_bytes>maximum.maximum.stack_bytes||
      row.resources.peak>maximum.max_peak_bytes)return false;
   std::size_t reservation=0;
   if(!add(reservation,support.resources.plan_bytes)||!add(reservation,support.resources.payload_bytes)||
      !add(reservation,support.resources.scratch_bytes)||!add(reservation,support.resources.stack_bytes)||reservation>row.resources.peak)return false;
  }
  // Admission charges the complete owning node/parameter/contract/edge ledger.
  // Eight wire bytes and four arena bytes per admitted owning byte conservatively
  // cover schema keys, length markers, enum varints and projection records. This
  // includes a graph in a capabilities reply and maximal response scalars, before
  // any service mutation, for BOTH codecs. Composition must choose a fitting
  // admission profile rather than discovering an oversized reply after Apply.
  if(c.acquire_staging) {StagingLease lease(c,{});if(!lease.valid)return false;}
  nc::CborCodec cb;nc::ProtobufCodec pb;
  for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cb),static_cast<const nc::Codec*>(&pb)}) {
   const auto footprint=codec->measure(m,l);
   if(!footprint)return false;
   if(c.runtime_preflights_reply_envelope)continue;
   std::size_t wire_bound=4096,arena_bound=footprint.required_arena,text_bound=footprint.required_wire;
   if(!add(wire_bound,footprint.required_wire)||!product(wire_bound,8,admission.max_owned_metadata_bytes)||
      !product(arena_bound,4,admission.max_owned_metadata_bytes)||!add(text_bound,admission.max_owned_metadata_bytes)||
      wire_bound>l.wire_bytes||arena_bound>l.arena_bytes||text_bound>l.total_text_bytes)return false;
  }
  if(!c.runtime_preflights_reply_envelope&&(admission.max_owned_metadata_bytes>l.total_text_bytes||
     admission.max_owned_metadata_bytes/sizeof(domain::ProcessingPort)>l.ports||
     admission.max_owned_metadata_bytes/sizeof(domain::ProcessingParameter)>l.parameters||
     admission.max_owned_metadata_bytes/4>l.records))return false;
 }
 return true;
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
  const auto& l=config.codec_limits;effective_bytes=l.wire_bytes;effective_packet_bytes=config.max_packet_bytes;
  incoming.resize(config.max_packet_bytes+4);outgoing.resize(*control_framed_bound(config.max_packet_bytes));read.resize(config.read_chunk_bytes);
  encoded.resize(l.wire_bytes);assembly.resize(l.wire_bytes);
  fragment.resize(fragment_chunk(config.max_packet_bytes)+ControlFragmentHeaderBytes);
  const auto words=(l.arena_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t);
  arena_storage.resize(words); staging_storage.resize(config.acquire_staging?0:words);
  arena={reinterpret_cast<std::byte*>(arena_storage.data()),l.arena_bytes};
  staging={reinterpret_cast<std::byte*>(staging_storage.data()),staging_storage.size()*sizeof(std::max_align_t)};
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
  auto bound=control_message_bound(payload.size(),effective_packet_bytes);const auto packet=control_framed_bound(effective_packet_bytes);
  if(!bound||!packet||!add(*bound,*packet)||!budget(*bound))return false;
  if(payload.size()>effective_packet_bytes){if(tx_sequence==UINT64_MAX)return false;++tx_sequence;}
  if(payload.data()!=encoded.data())std::memcpy(encoded.data(),payload.data(),payload.size());
  tx_kind=id;tx_size=payload.size();tx_offset=0;return true;
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
  if(tx_size<=effective_packet_bytes) {
   if(packets->enqueue(tx_kind,std::span<const std::uint8_t>(encoded).first(tx_size))!=Status::ok)return false;
   packet_kind=tx_kind;
   tx_offset=tx_size;return true;
  }
  const auto count=std::min(fragment_chunk(effective_packet_bytes),tx_size-tx_offset);std::size_t at=0;
  put(fragment,at,1,2);put(fragment,at,context.session,8);put(fragment,at,tx_sequence,8);
  put(fragment,at,tx_size,4);put(fragment,at,tx_offset,4);put(fragment,at,tx_kind,1);put(fragment,at,count,2);
  std::memcpy(fragment.data()+at,encoded.data()+tx_offset,count);
  if(packets->enqueue(ControlFragmentId,std::span<const std::uint8_t>(fragment).first(at+count))!=Status::ok)return false;
  packet_kind=ControlFragmentId;
  tx_offset+=count;return true;
 }
 void ack(std::uint8_t status,ControlSessionError reason,std::uint64_t request_nonce) {
  // A rejection is sent in the local fixed envelope, even when the proposed
  // peer limit was zero/too small. There is no negotiated payload limit yet.
  if(status)effective_bytes=config.codec_limits.wire_bytes;
  std::size_t at=0;
  put(assembly,at,HelloVersion,2);put(assembly,at,request_nonce,8);put(assembly,at,status,1);
  put(assembly,at,status?0:config.schema_version,4);put(assembly,at,status?0:(*selected==nc::CodecId::Cbor?1:2),1);
  put(assembly,at,status?0:effective_bytes,4);put(assembly,at,status?0:effective_packet_bytes,4);put(assembly,at,status?0:config.allow_mutations,1);
  put(assembly,at,status?0:context.client,8);put(assembly,at,status?0:context.session,8);put(assembly,at,status?0:context.epoch,8);
  put_text(assembly,at,config.device_identity);put_text(assembly,at,{});
  if(!queue(ControlHelloAckId,std::span<const std::uint8_t>(assembly).first(at))){fail(ControlSessionError::LimitExceeded);return;}
  if(status){error=reason;state=ControlSessionState::Negotiating;handshake_at=clock.now_ms();reject_after_write=true;}
  else state=ControlSessionState::Ready;
 }
 void hello(std::span<const std::uint8_t> bytes) {
  Reader r{bytes};const auto version=r.number(2),n=r.number(8),schema=r.number(4),mask=r.number(4),maximum=r.number(4),packet_maximum=r.number(4);
  const auto device=r.text(256);
  if(bytes.size()>512||!r.end()||version!=HelloVersion||packet_maximum<512||packet_maximum>65535||!n||n<=last_nonce||state!=ControlSessionState::Disconnected){fail(ControlSessionError::Malformed);return;}
  last_nonce=n;effective_packet_bytes=std::min(config.max_packet_bytes,std::size_t(packet_maximum));
  if(schema!=config.schema_version){ack(2,ControlSessionError::Unsupported,n);return;}
  if(device!=config.device_identity){ack(8,ControlSessionError::WrongDevice,n);return;}
  selected=nc::negotiate(config.schema_version,std::uint32_t(schema),config.codec_mask,std::uint32_t(mask));
  if(!selected){ack(3,ControlSessionError::Unsupported,n);return;}
  effective_bytes=std::min(config.codec_limits.wire_bytes,std::size_t(maximum));
  if(effective_bytes<config.codec_limits.wire_bytes){ack(4,ControlSessionError::LimitExceeded,n);return;}
  const auto opened=service->open_session(config.client_identity);
  if(!opened){ack(opened.error==application::ControlError::busy?6:4,ControlSessionError::Busy,n);return;}
  context=opened.identity;nonce=n;ack(0,ControlSessionError::None,n);
 }
 void hello_ack(std::span<const std::uint8_t> bytes) {
  Reader r{bytes};const auto version=r.number(2),n=r.number(8),status=r.number(1),schema=r.number(4),id=r.number(1),maximum=r.number(4),packet_maximum=r.number(4),writable=r.number(1);
  application::SessionIdentity issued{r.number(8),r.number(8),r.number(8)};
  const auto device=r.text(256);const auto diagnostic=r.text(256,true);(void)diagnostic;
  if(bytes.size()>1024||!r.end()||version!=HelloVersion||n!=nonce||state!=ControlSessionState::Negotiating||status>8||writable>1){fail(ControlSessionError::Malformed);return;}
  if(status){
   if(schema||id||maximum||packet_maximum||writable||issued.client||issued.session||issued.epoch){fail(ControlSessionError::Malformed);return;}
   fail(status==8?ControlSessionError::WrongDevice:status==4?ControlSessionError::LimitExceeded:status==6?ControlSessionError::Busy:ControlSessionError::Unsupported);return;
  }
  if(device!=config.device_identity){fail(ControlSessionError::WrongDevice);return;}
  const auto mask=id==1?nc::CborMask:id==2?nc::ProtobufMask:0;
  auto chosen=nc::negotiate(config.schema_version,std::uint32_t(schema),config.codec_mask,mask);
  if(!chosen||!maximum||maximum>config.codec_limits.wire_bytes||packet_maximum<512||packet_maximum>config.max_packet_bytes||!issued.client||!issued.session||!issued.epoch){fail(ControlSessionError::Malformed);return;}
  selected=chosen;context=issued;effective_bytes=std::size_t(maximum);effective_packet_bytes=std::size_t(packet_maximum);config.allow_mutations=bool(writable);state=ControlSessionState::Ready;
 }
 void message(std::uint8_t id,std::span<const std::uint8_t> bytes) {
  if(state!=ControlSessionState::Ready||!selected||id!=(service?ControlCommandId:ControlReplyId)){fail(ControlSessionError::Unsupported);return;}
  if(service&&(tx_size||packets->pending_bytes())) {
   if(deferred_bytes||bytes.size()>assembly.size()){fail(ControlSessionError::Busy);return;}
   if(bytes.data()!=assembly.data())std::memcpy(assembly.data(),bytes.data(),bytes.size());
   deferred_bytes=bytes.size();return;
  }
  nc::Message wire;
  const auto result=[&] {
   StagingLease lease(config,staging);
   if(!lease.valid)return nc::Result{nc::Status::BufferTooSmall};
   if(overlaps(lease.bytes,arena)||overlaps(lease.bytes,std::as_bytes(bytes))||
      overlaps(lease.bytes,std::as_bytes(std::span(encoded)))||overlaps(lease.bytes,std::as_bytes(std::span(assembly))))
    return nc::Result{nc::Status::InvalidArgument};
   return codec().decode(bytes,arena,lease.bytes,wire,limits());
  }(); // Staging views are gone; mapped request views live only in primary arena.
  if(!result){fail(ControlSessionError::Codec);return;}
  if(application::SessionIdentity{wire.client,wire.session,wire.epoch}!=context){fail(ControlSessionError::Identity);return;}
  if(service) {
   const auto* command=std::get_if<nc::Command>(&wire.body);
   if(!command||!wire.request){fail(ControlSessionError::Malformed);return;}
   if(wire.request<highest_request){fail(ControlSessionError::Replay);return;}
   if(!config.allow_mutations&&command->id!=nc::CommandId::State&&command->id!=nc::CommandId::Capabilities){fail(ControlSessionError::ReadOnly);return;}
   application::ProcessingDTO dto; std::string graph_id(command->graph_id);
   application::ProcessingCommand mapped;
   switch(command->id) {
   case nc::CommandId::Capabilities:mapped=application::GetCapabilities{};break;
   case nc::CommandId::State:mapped=application::GetState{};break;
   case nc::CommandId::Prepare: {
    domain::NumericProcessingGraph graph;
    if(!own_graph(*command->graph,limits(),graph)){fail(ControlSessionError::Mapping);return;}
    dto.numeric=std::move(graph);mapped=application::PrepareProcessing{&dto};break;
   }
   case nc::CommandId::Apply:mapped=application::ApplyProcessing{command->token,graph_id};break;
   case nc::CommandId::Release:mapped=application::ReleasePrepared{command->token};break;
   case nc::CommandId::SetParameters: {
    application::SetNumericProcessingParameters p;p.node=command->node;p.enabled=command->enabled;
    if(!own_parameters(*command->parameters,limits(),p.parameters)){fail(ControlSessionError::Mapping);return;}
    mapped=std::move(p);break;
   }
   case nc::CommandId::Start:case nc::CommandId::Stop:fail(ControlSessionError::Mapping);return;
   case nc::CommandId::Persist:mapped=application::ExplicitPersist{command->graph_revision};break;
   }
   const auto bound=control_message_bound(effective_bytes,effective_packet_bytes);
   const auto packet=control_framed_bound(effective_packet_bytes);std::size_t reservation=bound?*bound:0;
   if(!bound||!packet||!add(reservation,*packet)||!budget(reservation)){fail(ControlSessionError::LimitExceeded);return;}
   highest_request=std::max(highest_request,wire.request);
   auto reply=service->submit(context,{wire.request,wire.expected_revision},mapped);
   if(commit_handler){in_callback=true;commit_handler(reply,commit_context);in_callback=false;}
   nc::Reply r;r.command=command->id;r.error=reply_error(reply.error);
   r.state.revision=reply.state.revision;r.state.run_intent=reply.state.run_requested;
   r.state.token=reply.prepared_token?reply.prepared_token:reply.state.own_prepared_token;
   if(reply.numeric_resources) {
    const auto& a=*reply.numeric_resources;const auto& resources=a.resources;
    r.state.resources={resources.plan_bytes,resources.payload_bytes,resources.scratch_bytes,resources.stack_bytes,a.peak_bytes};
   }
   if(!reply.numeric_validation) {
    if(reply.numeric_validation.node>=0)r.diagnostic.node=reply.numeric_validation.node;
    switch(reply.numeric_validation.error) {
    case domain::ProcessingError::InvalidPort:case domain::ProcessingError::InvalidContract:
    case domain::ProcessingError::IncompatiblePorts:r.diagnostic.port=reply.numeric_validation.port;break;
    default:break;
    }
    switch(reply.numeric_validation.error) {
    case domain::ProcessingError::MissingEndpoint:case domain::ProcessingError::MultipleInputs:
    case domain::ProcessingError::IncompatiblePorts:r.diagnostic.edge=reply.numeric_validation.edge;break;
    default:break;
    }
   }
   switch(reply.state.lifecycle) {
    case application::LifecycleState::Empty:r.state.lifecycle=nc::Lifecycle::Empty;break;
    case application::LifecycleState::Ready:r.state.lifecycle=nc::Lifecycle::Prepared;break;
    case application::LifecycleState::Running:r.state.lifecycle=nc::Lifecycle::Running;break;
    case application::LifecycleState::Stopped:r.state.lifecycle=nc::Lifecycle::Stopped;break;
   }
   GraphProjection graph;
   if(reply.state.applied_numeric) {
    if(!graph.assign(*reply.state.applied_numeric,limits())){fail(ControlSessionError::Mapping);return;}
    r.state.applied=graph.wire();
   }
   if(command->id==nc::CommandId::Capabilities)r.capabilities=*config.advertised_capabilities;
   nc::Message out;out.client=context.client;out.session=context.session;out.epoch=context.epoch;
   out.request=wire.request;out.expected_revision=wire.expected_revision;out.body=r;
   // Application graph revision differs from the control revision on Start/Stop.
   // Keep both exact through the codec's bounded optional extension namespace.
   std::array<std::uint8_t,16> revisions{};std::size_t revision_at=0;
   put(revisions,revision_at,reply.state.graph_revision,8);put(revisions,revision_at,reply.state.persisted_graph_revision,8);
   nc::Extension revision_extension{1024,revisions};out.extensions=std::span(&revision_extension,1);
   if(!encode_message(out,ControlReplyId))fail(ControlSessionError::Codec);
   else if(handler){in_callback=true;handler(out,handler_context);in_callback=false;}
  } else {
   const auto* reply=std::get_if<nc::Reply>(&wire.body);
   if(!reply||!pending_request||wire.request!=pending_request||reply->command!=pending_command){fail(ControlSessionError::Identity);return;}
   pending_request=0;
   if(handler){in_callback=true;handler(wire,handler_context);in_callback=false;}
  }
 }

 void packet(PacketView view) {
  if(state==ControlSessionState::Failed)return;
  if(view.payload.size()>effective_packet_bytes){fail(ControlSessionError::LimitExceeded);return;}
  if(total && view.id!=ControlFragmentId){fail(ControlSessionError::Malformed);return;}
  if(view.id==ControlHelloId&&service){hello(view.payload);return;}
  if(view.id==ControlHelloAckId&&!service){hello_ack(view.payload);return;}
  if(view.id!=ControlCommandId&&view.id!=ControlReplyId&&view.id!=ControlFragmentId&&
     state==ControlSessionState::Ready&&packet_handler) {
   in_callback=true;packet_handler(view,packet_context);in_callback=false;return;
  }
  if(view.id!=ControlFragmentId){message(view.id,view.payload);return;}
  if(state!=ControlSessionState::Ready){fail(ControlSessionError::Unsupported);return;}
  Reader r{view.payload};const auto version=r.number(2),session=r.number(8),sequence=r.number(8),length=r.number(4),offset=r.number(4),kind=r.number(1),count=r.number(2);
  if(!r.valid||version!=1||session!=context.session||!sequence||!count||count>fragment_chunk(effective_packet_bytes)||
     count!=view.payload.size()-r.at||length<=effective_packet_bytes||length>effective_bytes||offset>length||count>length-offset||
     kind!=(service?ControlCommandId:ControlReplyId)){fail(ControlSessionError::Malformed);return;}
  if(!total){
   // A deferred complete request owns RX until the outstanding TX drains.
   if(deferred_bytes){fail(ControlSessionError::Busy);return;}
   if(offset||sequence<=last_assembly_sequence){fail(ControlSessionError::Replay);return;}
   if(!control_peer_budget_fits(config.peer_budget_bytes,device_pending,packets->pending_bytes(),std::size_t(length),tx_size?*control_message_bound(tx_size,effective_packet_bytes):0)){fail(ControlSessionError::LimitExceeded);return;}
   total=std::size_t(length);received=0;assembly_sequence=sequence;assembly_kind=std::uint8_t(kind);assembly_at=clock.now_ms();
  }
  if(sequence!=assembly_sequence||kind!=assembly_kind||length!=total||offset!=received||
     clock.now_ms()<assembly_at||clock.now_ms()-assembly_at>=config.timeout_ms){fail(ControlSessionError::Malformed);return;}
  std::memcpy(assembly.data()+received,view.payload.data()+r.at,std::size_t(count));received+=std::size_t(count);
  if(received==total){
   const auto complete=total;const auto completed_kind=assembly_kind;
   last_assembly_sequence=assembly_sequence;total=received=0;assembly_sequence=assembly_at=0;
   message(completed_kind,std::span<const std::uint8_t>(assembly).first(complete));
  }
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
 auto& d=*impl_;if(d.in_callback||d.service||d.state!=ControlSessionState::Ready)return Status::disconnected;
 if(d.pending_request||d.tx_size||d.packets->pending_bytes()||d.total||d.deferred_bytes)return Status::would_block;
 if(!request||request<d.highest_request)return Status::invalid_argument;
 if(command.graph && (domain::processing_versions_supported(command.graph->versions)!=Status::ok ||
    domain::processing_boundary_version_supported(domain::ProcessingBoundary::Control,command.graph->versions.control)!=Status::ok))
  return Status::unsupported_version;
 if(!d.config.allow_mutations&&command.id!=nc::CommandId::State&&command.id!=nc::CommandId::Capabilities){d.error=ControlSessionError::ReadOnly;return Status::unavailable;}
 nc::Message out;out.client=d.context.client;out.session=d.context.session;out.epoch=d.context.epoch;
 out.request=request;out.expected_revision=expected_revision;out.body=command;
 if(!d.encode_message(out,ControlCommandId))return Status::exhausted;
 d.highest_request=std::max(d.highest_request,request);d.pending_request=request;d.pending_command=command.id;
 d.request_at=d.clock.now_ms();return Status::ok;
}
Status ControlSession::poll(ByteTransport& transport,std::int64_t device_pending) {
 auto& d=*impl_;if(d.in_callback)return Status::invalid_argument;
 if(!d.packets||d.state==ControlSessionState::Failed)return Status::disconnected;
 if(transport.state()!=TransportState::connected||device_pending<0){d.fail(ControlSessionError::Disconnected);return Status::disconnected;}
 d.device_pending=std::size_t(device_pending);
 const auto now=d.clock.now_ms();
 auto expired=[&](std::uint64_t at){return now<at||now-at>=d.config.timeout_ms;};
 if((d.state==ControlSessionState::Negotiating&&expired(d.handshake_at))||(d.total&&expired(d.assembly_at))||(d.pending_request&&expired(d.request_at))){d.fail(ControlSessionError::Timeout);return Status::unavailable;}
 // submit() owns expiry and its rejection diagnostic. Expiring here would
 // discard the token provenance immediately before an arriving Apply.
 auto reservation=d.tx_size?control_message_bound(d.tx_size,d.effective_packet_bytes):std::optional<std::size_t>(0);
 if(!reservation||!d.budget(*reservation)){d.fail(ControlSessionError::LimitExceeded);return Status::exhausted;}
 const auto input=transport.read(d.read);
 if(input.transferred>d.read.size()){d.fail(ControlSessionError::Transport);return Status::invalid_argument;}
 if(input.transferred)d.packets->feed(std::span<const std::uint8_t>(d.read).first(input.transferred));
 if(input.status!=Status::ok&&input.status!=Status::would_block){d.fail(ControlSessionError::Transport);return input.status;}
 if(d.state==ControlSessionState::Failed)return Status::malformed;
 if(!d.emit()){d.fail(ControlSessionError::Transport);return Status::exhausted;}
 const auto output=d.packets->flush(transport);
 if(output.status!=Status::ok&&output.status!=Status::would_block){d.fail(ControlSessionError::Transport);return output.status;}
 if(!d.packets->pending_bytes())d.packet_kind=0;
 if(!d.packets->pending_bytes()&&d.tx_size&&d.tx_offset==d.tx_size){d.tx_size=d.tx_offset=0;if(d.reject_after_write){const auto error=d.error;d.fail(error);}}
 if(d.state==ControlSessionState::Ready&&d.deferred_bytes&&!d.tx_size&&!d.packets->pending_bytes()) {
  const auto n=d.deferred_bytes;d.deferred_bytes=0;d.message(ControlCommandId,std::span<const std::uint8_t>(d.assembly).first(n));
 }
 return Status::ok;
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





