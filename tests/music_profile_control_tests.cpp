#include "numeric_transport_fixture.h"
#include <control/numeric_remote_support.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <project_storage/numeric_array_codec.h>
#include <bit>
#include <vector>

namespace {
using namespace transport_fixture;namespace nc=bdsp_protocol::numeric_control;
constexpr std::size_t OneMiB=1024*1024;
domain::NumericProcessingGraph profile_graph(){
    static constexpr std::array<std::uint32_t,1> mono{0};NumericContract c;c.channel_order=mono;c.dtype=DType::Float32;
    c.shape={Rank::Vector,{Axis::Bin,Axis::None},{ExtentBounds{2049,2049},ExtentBounds{}}};c.clock={"profile-control",{1,32768},32768};
    c.quantity=Quantity::SpectrumPower;c.unit=NumericUnit::SquaredAmplitude;c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{8,1}};c.representation.alignment=4;
    domain::OwnedProcessingContract owner;require(domain::own_processing_contract(c,remote_numeric::control_limits().frame,16384,owner)==Status::ok,"own power contract");
    domain::ConfiguredProcessingNode source;source.id=0;source.kind="NetworkInput";source.outputs={{0,domain::PortDirection::Output,owner}};
    domain::ConfiguredProcessingNode normalizer;normalizer.id=1;normalizer.kind="FrequencyNormalizer";
    normalizer.parameters=domain::TransformProcessingParameters{{
        {"mode",std::string{"off"}},
        {"gains",domain::NumericArray{std::vector<float>(2049,1.0f)}},
        {"sample_rate",NumericLiteral{std::uint64_t{32768}}},
        {"fft_size",NumericLiteral{std::uint64_t{4096}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}}}};
    normalizer.inputs={{0,domain::PortDirection::Input,owner}};normalizer.outputs={{0,domain::PortDirection::Output,owner}};
    domain::ConfiguredProcessingNode sink;sink.id=2;sink.kind="NetworkOutput";sink.inputs={{0,domain::PortDirection::Input,owner}};
    domain::NumericProcessingGraph graph;graph.graph_id="maximum-profile-control";graph.nodes={source,normalizer,sink};graph.edges={{0,1},{1,2}};return graph;
}
const std::vector<float>& profile_values(const domain::NumericProcessingGraph& graph) {
    for(const auto& node:graph.nodes)if(node.kind=="FrequencyNormalizer")
        for(const auto& parameter:std::get<domain::TransformProcessingParameters>(node.parameters).values)
            if(parameter.key=="gains")return std::get<domain::NumericArray>(parameter.value).values;
    throw std::runtime_error("Missing typed profile coefficients");
}

domain::NumericProcessingGraph decoded_graph(const pipeline::EngineSnapshot& snapshot){domain::NumericProcessingGraph graph;require(bool(desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),graph)),"decode profile CONTROL snapshot");return graph;}
void codec_envelope(const domain::NumericProcessingGraph& graph,const application::ProcessingCapabilities& capabilities){
    const auto limits=remote_numeric::control_limits();require(limits.string_bytes==OneMiB && limits.wire_bytes==4*OneMiB && limits.arena_bytes==4*OneMiB,"bounded PC profile envelope");
    require(nc::device_limits().wire_bytes==16384 && nc::device_limits().string_bytes==64,"device limits unchanged");
    remote_numeric::CapabilitySnapshot advertised;require(advertised.assign(graph,capabilities)==Status::ok,"large profile capability projection");
    const auto envelope=control::numeric::reply_envelope_fits(graph,limits,advertised.value);require(bool(envelope),"full State/capability/diagnostic envelope fits");
    require(envelope.required_text>=3*project_storage::encode_array_generator_values(profile_values(graph)).size() && envelope.required_wire<=limits.wire_bytes && envelope.required_arena<=limits.arena_bytes,"full envelope measured including repeated profile");
    control::numeric::GraphProjection projected;require(bool(projected.assign(graph,limits)),"project typed coefficient array");
    std::string diagnostic(OneMiB,'x');nc::Reply reply;reply.command=nc::CommandId::Capabilities;reply.state.applied=projected.wire();reply.capabilities=advertised.value;reply.diagnostic.parameter=diagnostic;
    nc::Message message;message.body=reply;
    std::vector<std::uint8_t> wire(limits.wire_bytes),scratch(limits.wire_bytes);
    std::vector<std::max_align_t> arena((limits.arena_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t)),stage(arena.size());
    nc::CborCodec cbor;nc::ProtobufCodec protobuf;
    for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}){
        const auto encoded=codec->encode(message,wire,scratch,limits);require(bool(encoded),"encode typed profile reply");nc::Message decoded;
        require(bool(codec->decode(std::span(wire).first(encoded.produced),{reinterpret_cast<std::byte*>(arena.data()),limits.arena_bytes},{reinterpret_cast<std::byte*>(stage.data()),limits.arena_bytes},decoded,limits)),"decode typed profile reply");
        const auto& restored=std::get<nc::Reply>(decoded.body);require(restored.state.applied.has_value() && restored.capabilities.has_value() && restored.diagnostic.parameter==diagnostic,"reply fields preserved");
        domain::NumericProcessingGraph owned;require(bool(control::numeric::own_graph(*restored.state.applied,limits,owned)) && profile_values(owned)==profile_values(graph),"typed profile coefficients round-trip exactly");
        require(restored.capabilities->rows.size()==3,"capabilities retained");
        auto too_short=limits;too_short.string_bytes=project_storage::encode_array_generator_values(profile_values(graph)).size()-1;require(!codec->measure(message,too_short),"overlong field refused before encoding");
        std::printf("profile CONTROL codec=%u wire=%zu arena=%zu full_envelope_wire=%zu full_envelope_arena=%zu text=%zu\n",unsigned(codec->id()),encoded.produced,encoded.required_arena,envelope.required_wire,envelope.required_arena,envelope.required_text);
    }
    auto config=remote_numeric::desktop_session_config();config.endpoint_authorized=true;config.device_identity="numeric-desktop";
    require(config.read_chunk_bytes==65535 && config.timeout_ms==5000,"packet-sized PC reads keep original session deadline");
    const auto storage=control::numeric::ControlSession::storage_requirements(config);require(storage.has_value() && storage->persistent_bytes<=config.storage_budget_bytes,"actual session storage fits 17MiB budget");
    std::printf("profile CONTROL storage=%zu budget=%zu peer=%zu reserved_four_peers=%zu\n",storage->persistent_bytes,config.storage_budget_bytes,config.peer_budget_bytes,remote_numeric::ControlTransportReservationBytes);
}
void dump_remote(const char* phase,const NumericRemoteEndpoint& remote,const NumericEngine& engine,const NumericControlServer& server){
    const auto snapshot=remote.snapshot();std::fprintf(stderr,"profile CONTROL phase=%s remote_epoch=%llu remote_graph_revision=%llu local_graph_revision=%llu graph_empty=%d warning=%s server=%s diagnostics=%s\n",
        phase,static_cast<unsigned long long>(snapshot.epoch),static_cast<unsigned long long>(snapshot.graph_revision),static_cast<unsigned long long>(engine.snapshot().graph_revision),snapshot.graph.isEmpty(),
        qPrintable(remote.network_warning()),qPrintable(server.error_string()),QJsonDocument(snapshot.diagnostics).toJson(QJsonDocument::Compact).constData());
}
EngineReply profile_apply(Replies& replies,NumericRemoteEndpoint& remote,NumericEngine& engine,const NumericControlServer& server,EngineCommand command){
    const auto request=remote.submit(std::move(command));
    // Public ApplyGraph can queue behind one in-flight State request, then
    // completes Prepare and Apply. Each retains the SessionConfig deadline.
    const auto deadline=int(3*remote_numeric::desktop_session_config().timeout_ms);
    const bool ready=wait([&]{return replies.values.contains(request);},deadline);
    if(!ready)dump_remote("queued State+Prepare+Apply deadline",remote,engine,server);
    require(ready,"large profile Prepare+Apply reply deadline");auto reply=std::move(replies.values.at(request));replies.values.erase(request);return reply;
}
void global_peer_bound(NumericEngine& engine){
    engine.with_control_service([&](auto&){
        std::array<std::uint64_t,remote_numeric::ControlMaximumPeers> checks{};
        for(auto& token:checks){token=engine.register_control_reply_check([](const auto&){return true;});require(token!=0,"four globally bounded transport check registrations");}
        require(engine.register_control_reply_check([](const auto&){return true;})==0,"fifth transport registration refused across server instances");
        engine.unregister_control_reply_check(checks[0]);checks[0]=engine.register_control_reply_check([](const auto&){return true;});require(checks[0]!=0,"released transport reservation reusable");
        for(const auto token:checks)engine.unregister_control_reply_check(token);
    });
}
void application_bounds(NumericEngine& engine,const domain::NumericProcessingGraph& graph){
    const auto caps=engine.capabilities();require(caps.control.max_field_string_bytes==OneMiB,"application profile field bound");
    const auto snapshot=application::control_snapshot_bytes(caps);require(snapshot.has_value() && *snapshot<=caps.control.max_control_scratch_bytes && *snapshot<=caps.control.max_replay_bytes_per_session,"aggregate bound preserves replay/scratch budget");
    engine.with_control_service([&](auto& service){
        const auto opened=service.open_session(991);require(bool(opened),"open bounded application session");const auto before=service.observe_state();
        domain::TransformProcessingParameters too_large;
        for(unsigned i=0;i<5;++i)too_large.values.push_back({"gains",std::string(OneMiB,'x')});
        application::SetNumericProcessingParameters update{1,std::move(too_large),true};
        const auto refused=service.submit(opened.identity,{1,before.revision},update);
        require(refused.error==application::ControlError::limit_exceeded && service.observe_state().graph_revision==before.graph_revision,"aggregate oversized SetParameters rejected atomically");
        update.parameters=domain::TransformProcessingParameters{{{"gains",std::string(OneMiB+1,'x')}}};
        const auto field_refused=service.submit(opened.identity,{2,before.revision},update);
        require(field_refused.error==application::ControlError::limit_exceeded && service.observe_state().graph_revision==before.graph_revision,"single overlong field rejected atomically");
        require(service.observe_state().applied_numeric && profile_values(*service.observe_state().applied_numeric)==profile_values(graph),"rejected update preserved exact working profile");
        service.close_session(opened.identity);
    });
}
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
    auto graph=profile_graph();NumericEngine engine;Replies local(engine);local.accepted(apply(graph));
    require(profile_values(decoded_graph(engine.snapshot()))==profile_values(graph),"local Apply supports typed profile");codec_envelope(graph,engine.capabilities());application_bounds(engine,graph);global_peer_bound(engine);
    NumericControlServer server(engine);require(server.listen(QHostAddress::LocalHost,0),"bounded desktop CONTROL server");NumericRemoteEndpoint remote("127.0.0.1",server.port());Replies replies(remote);
    remote.connect_remote();const auto boot_deadline=int(2*remote_numeric::desktop_session_config().timeout_ms);
    const bool negotiated=wait([&]{return remote.snapshot().graph_revision==engine.snapshot().graph_revision&&!remote.snapshot().graph.isEmpty();},boot_deadline);
    if(!negotiated)dump_remote("HELLO+Capabilities deadline",remote,engine,server);
    require(negotiated,"fragmented large State/capability negotiation");
    require(profile_values(decoded_graph(remote.snapshot()))==profile_values(graph),"remote State profile exact");
    auto changed=graph;
    auto& gains=std::get<domain::NumericArray>(std::get<domain::TransformProcessingParameters>(changed.nodes[1].parameters).values[1].value).values;
    gains[2]=std::bit_cast<float>(std::uint32_t{0x3f800001});
    const auto reply=profile_apply(replies,remote,engine,server,apply(changed));if(!reply.accepted)std::fprintf(stderr,"remote typed profile rejected: %s\n",qPrintable(reply.error));require(reply.accepted,"remote typed profile Apply");
    require(wait([&]{return profile_values(decoded_graph(remote.snapshot()))==profile_values(changed);},10000),"remote large reply preserved mutated bytes");
    require(profile_values(decoded_graph(engine.snapshot()))==profile_values(changed),"server working profile exact after remote Apply");
    remote.disconnect_remote();server.close();std::puts("music_profile_control_tests PASS: typed coefficient local/TCP Apply+State, CBOR/Protobuf full envelope, atomic overlimit rejection");return 0;
}catch(const std::exception& error){std::fprintf(stderr,"FAIL profile CONTROL: %s\n",error.what());return 1;}}
