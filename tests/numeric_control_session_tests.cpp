#include <control/numeric_processing_message_mapper.h>
#include <control/numeric_control_session.h>
#include <bdsp_protocol/numeric_control_codec.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <limits>
#include <deque>
#include <algorithm>
#include <bit>
namespace nc=bdsp_protocol::numeric_control;
using namespace signal_contracts;
bool native_scenario=false;
void mapper_cases() {
    const auto limits=nc::desktop_limits();
    std::array<std::uint32_t,2> channels{3,17};
    std::string clock="source.clock",identity="exact.graph";
    std::array<nc::Parameter,4> values{{{"minimum",NumericLiteral{INT64_MIN}},
        {"maximum",NumericLiteral{UINT64_MAX}},{"precision",NumericLiteral{9007199254740993ULL}},
        {"endpoint",std::string_view{"remote.sensor"}}}};
    for(const auto& dt:dtype_descriptors()) for(auto rank:{Rank::Scalar,Rank::Vector,Rank::Matrix}) {
        NumericContract c;c.dtype=dt.type;c.shape.rank=rank;c.semantic=Semantic::Numeric;
        c.clock.domain=clock;c.clock.tick_seconds={1,48000};c.clock.sample_rate=48000;
        c.channel_order=rank==Rank::Scalar?std::span(channels).first(1):std::span(channels);
        c.representation.encoding=StorageEncoding::FixedBigEndian;
        c.representation.alignment=dt.alignment;
        if(rank!=Rank::Scalar) {c.shape.axes[0]=Axis::Declared;c.shape.extents[0]={3,7};}
        if(rank==Rank::Matrix) {c.shape.axes[1]=Axis::Declared;c.shape.extents[1]={2,5};}
        nc::Port port{0,domain::PortDirection::Output,c};
        nc::Node node;node.id=1;node.kind="NetworkInput";node.parameters=nc::EndpointParameters{values};
        node.outputs=std::span(&port,1);
        nc::Graph graph{identity,{},std::span(&node,1),{},{1,125}};
        domain::NumericProcessingGraph owned;
        assert(control::numeric::own_graph(graph,limits,owned));
        assert(same_contract(c,owned.nodes[0].outputs[0].contract.view()));
        assert(std::get<std::uint64_t>(std::get<NumericLiteral>(std::get<domain::EndpointProcessingParameters>(owned.nodes[0].parameters).values[1].value))==UINT64_MAX);
        control::numeric::GraphProjection projection;assert(projection.assign(owned,limits));
        auto moved=std::move(projection);owned={};
        assert(same_contract(c,moved.wire().nodes[0].outputs[0].contract));
        nc::Message message;nc::Command command;command.id=nc::CommandId::Prepare;command.graph=moved.wire();message.body=command;
        std::vector<std::uint8_t> bytes(limits.wire_bytes),scratch(limits.wire_bytes);
        std::vector<std::max_align_t> a((limits.arena_bytes+sizeof(std::max_align_t)-1)/sizeof(std::max_align_t)),s(a.size());
        nc::CborCodec cbor;nc::ProtobufCodec protobuf;
        for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cbor),static_cast<const nc::Codec*>(&protobuf)}) {
            auto e=codec->encode(message,bytes,scratch,limits);assert(e);
            nc::Message decoded;
            assert(codec->decode(std::span(bytes).first(e.produced),{reinterpret_cast<std::byte*>(a.data()),limits.arena_bytes},
                {reinterpret_cast<std::byte*>(s.data()),limits.arena_bytes},decoded,limits));
            domain::NumericProcessingGraph second;assert(control::numeric::own_graph(*std::get<nc::Command>(decoded.body).graph,limits,second));
            assert(same_contract(c,second.nodes[0].outputs[0].contract.view()));
            assert(second.graph_id==identity && second.execution_interval.numerator==1 && second.execution_interval.denominator==125);
            decoded={};std::fill(a.begin(),a.end(),std::max_align_t{});
            assert(std::get<NumericContract>(second.nodes[0].outputs[0].contract.view()).clock.domain==clock);
            const auto& p=std::get<domain::EndpointProcessingParameters>(second.nodes[0].parameters).values;
            assert(std::get<std::uint64_t>(std::get<NumericLiteral>(p[2].value))==9007199254740993ULL);
        }
        domain::NumericProcessingGraph retained;retained.graph_id="unchanged";
        graph.versions.processing=99;assert(!control::numeric::own_graph(graph,limits,retained));assert(retained.graph_id=="unchanged");
    }
    nc::Port p{0,domain::PortDirection::Output,StructuredContract{"custom.packet",7,"little.endian",{4,44}}};
    nc::Node n;n.id=1;n.kind="NetworkInput";n.outputs=std::span(&p,1);
    nc::Graph g{"structured",{},std::span(&n,1),{}};domain::NumericProcessingGraph out;
    assert(control::numeric::own_graph(g,limits,out));
    assert(same_contract(p.contract,out.nodes[0].outputs[0].contract.view()));
    domain::ConverterProcessingParameters policy;
    policy.policy.rounding=Rounding::TowardZero;policy.policy.overflow=Overflow::Saturate;
    policy.policy.precision_loss=PrecisionLoss::AllowDeclared;policy.policy.nonfinite=NonFinite::ReplaceFinite;
    policy.policy.scaling.mode=ScaleMode::DyadicAffine;
    policy.policy.scaling.gain={NumericLiteral{UINT64_MAX},-17};policy.policy.scaling.offset={NumericLiteral{INT64_MIN},7};
    policy.policy.nan_replacement=std::bit_cast<float>(std::uint32_t{0x80000000});
    nc::Parameters parameters=policy;domain::ProcessingParameters parameter_owner;
    assert(control::numeric::own_parameters(parameters,limits,parameter_owner));
    assert(domain::same_processing_parameters(domain::ProcessingParameters{policy},parameter_owner));
    domain::NumericProcessingGraph converter;converter.graph_id="converter";
    domain::ConfiguredProcessingNode node;node.id=1;node.kind="Converter";node.parameters=parameter_owner;
    NumericContract c;c.channel_order=std::span(channels).first(1);c.representation.encoding=StorageEncoding::FixedLittleEndian;
    domain::OwnedProcessingContract contract;assert(domain::own_processing_contract(c,limits.frame,4096,contract)==Status::ok);
    node.inputs.push_back({0,domain::PortDirection::Input,contract});node.outputs.push_back({0,domain::PortDirection::Output,contract});
    converter.nodes.push_back(std::move(node));control::numeric::GraphProjection projection;assert(projection.assign(converter,limits));
    nc::Message m;nc::Command cmd;cmd.id=nc::CommandId::Prepare;cmd.graph=projection.wire();m.body=cmd;
    std::vector<std::uint8_t> bytes(limits.wire_bytes),scratch(limits.wire_bytes);
    std::vector<std::max_align_t> arena(limits.arena_bytes/sizeof(std::max_align_t)+1),stage(arena.size());
    nc::CborCodec cb;nc::ProtobufCodec pb;
    for(const nc::Codec* codec:{static_cast<const nc::Codec*>(&cb),static_cast<const nc::Codec*>(&pb)}) {
        auto encoded=codec->encode(m,bytes,scratch,limits);assert(encoded);nc::Message decoded;
        assert(codec->decode(std::span(bytes).first(encoded.produced),{reinterpret_cast<std::byte*>(arena.data()),limits.arena_bytes},
            {reinterpret_cast<std::byte*>(stage.data()),limits.arena_bytes},decoded,limits));
        domain::NumericProcessingGraph restored;assert(control::numeric::own_graph(*std::get<nc::Command>(decoded.body).graph,limits,restored));
        assert(domain::same_processing_parameters(parameter_owner,restored.nodes[0].parameters));
    }
}
struct Clock final:application::ControlClock {
    std::uint64_t now=100;std::uint64_t now_ms() const noexcept override{return now;}
};
struct FaultRuntime final:application::ProcessingControlRuntimePort {
    std::shared_ptr<const domain::NumericProcessingGraph> pending,working;
    application::LifecycleState lifecycle=application::LifecycleState::Empty;
    bool intent=true;int prepares=0,applies=0;
    Status apply_status=Status::ok;
    domain::NumericDeviceCapabilities capabilities;
    std::optional<domain::ProcessingGraphAdmission> pending_admission,working_admission;
    application::LifecycleResult prepare_numeric_transactional(const domain::NumericProcessingGraph& g,const application::NumericRuntimeLimits& limits) override {
        std::vector<std::size_t> workspace(g.nodes.size()*2);domain::ProcessingGraphAdmission admitted;
        auto device=capabilities;device.profile=limits.resources;
        auto diagnostic=domain::admit_processing_graph(g,limits.admission,device,workspace,admitted);
        if(!diagnostic)return {Status::invalid_argument,diagnostic};
        ++prepares;pending=std::make_shared<const domain::NumericProcessingGraph>(g);pending_admission=admitted;return {};
    }
    application::LifecycleResult apply_transactional() override {
        ++applies;if(apply_status!=Status::ok)return {apply_status,{}};
        working=std::move(pending);pending.reset();working_admission=pending_admission;pending_admission.reset();
        intent=true;lifecycle=application::LifecycleState::Running;return {};
    }
    void discard_prepared() noexcept override{pending.reset();pending_admission.reset();}
    application::MemoryRequirements pending_memory_requirements() const noexcept override{return {64,128,64};}
    application::LifecycleState state() const noexcept override{return lifecycle;}
    const domain::NumericProcessingGraph* working_numeric_graph() const noexcept override{return working?&*working:nullptr;}
    std::shared_ptr<const domain::NumericProcessingGraph> working_numeric_snapshot() const noexcept override{return working;}
    std::optional<domain::ProcessingGraphAdmission> pending_numeric_admission() const noexcept override{return pending_admission;}
    std::optional<domain::ProcessingGraphAdmission> working_numeric_admission() const noexcept override{return working_admission;}
};
struct Transport final:bdsp_protocol::ByteTransport {
    std::deque<std::uint8_t> input;Transport* other=nullptr;bool connected=true,blocked=false;
    std::size_t segment=13;
    bdsp_protocol::TransportState state() const noexcept override{return connected?bdsp_protocol::TransportState::connected:bdsp_protocol::TransportState::disconnected;}
    bdsp_protocol::IoResult read(std::span<std::uint8_t> out) noexcept override {
        const auto count=std::min({out.size(),input.size(),segment});
        for(std::size_t i=0;i<count;++i){out[i]=input.front();input.pop_front();}return {Status::ok,count};
    }
    bdsp_protocol::IoResult write(std::span<const std::uint8_t> bytes) noexcept override {
        if(blocked)return {Status::would_block,0};
        const auto count=std::min(bytes.size(),segment);
        if(other)other->input.insert(other->input.end(),bytes.begin(),bytes.begin()+std::ptrdiff_t(count));
        return {Status::ok,count};
    }
};
struct Receipt {
    int calls=0;nc::Error error=nc::Error::None;std::uint64_t request=0,revision=0,token=0;
    bool intent=false,applied=false;std::size_t rows=0;
    domain::NumericProcessingGraph graph;
    nc::Resources resources;
    void take(const nc::Message& m) {
        ++calls;request=m.request;const auto& r=std::get<nc::Reply>(m.body);
        error=r.error;revision=r.state.revision;token=r.state.token;intent=r.state.run_intent;applied=r.state.applied.has_value();
        rows=r.capabilities?r.capabilities->rows.size():0;resources=r.state.resources;
        if(r.state.applied)assert(control::numeric::own_graph(*r.state.applied,nc::desktop_limits(),graph));
    }
};
struct Fixture {
    Clock clock;FaultRuntime runtime;application::ProcessingCapabilities caps;
    std::array<application::ControlSessionSlot,4> slots;
    application::ProcessingControlService service;
    std::array<std::uint32_t,1> channels{5};NumericContract contract;
    std::array<nc::Port,1> source_ports,sink_ports;
    std::array<nc::Node,2> nodes;std::array<domain::Edge,1> edges{{{1,2,0,0}}};
    std::array<nc::CapabilityRow,2> rows;nc::Capabilities advertised;
    Fixture():service(runtime,clock,profile(),slots,77) {
        contract.dtype=DType::UInt64;contract.semantic=Semantic::Scalar;contract.channel_order=channels;
        contract.representation.encoding=native_scenario?StorageEncoding::Native:StorageEncoding::FixedLittleEndian;
        contract.clock.domain="exact.clock";
        source_ports[0]={0,domain::PortDirection::Output,contract};sink_ports[0]={0,domain::PortDirection::Input,contract};
        nodes[0].id=1;nodes[0].kind="NetworkInput";nodes[0].outputs=source_ports;
        nodes[1].id=2;nodes[1].kind="NetworkOutput";nodes[1].inputs=sink_ports;
        for(std::size_t i=0;i<2;++i)rows[i]={nodes[i],"test.fault.port","host",ValueDomain::FiniteOnly,{64,128,64,32,288}};
        advertised.rows=rows;advertised.maximum={100000,100000,100000,100000,1000000};
    }
    const application::ProcessingCapabilities& profile() {
        caps.control.max_sessions=4;caps.control.max_replay_bytes_per_session=1024*1024;
        caps.control.max_replay_bytes_total=4*1024*1024;caps.control.max_control_scratch_bytes=1024*1024;caps.numeric.emplace();
        auto& c=*caps.numeric;c.runtime.admission={16,32,8,16,256,32768,nc::desktop_limits().frame};
        c.runtime.resources={{100000,100000,100000,100000},0,1000000};
        c.device.profile=c.runtime.resources;
        c.device.query=[](const void*,const domain::ResolvedProcessingNode& n,const domain::ProcessingResourceProfile&) noexcept {
            bool exact=n.kind=="NetworkInput"||n.kind=="NetworkOutput";
            for(const auto ports:{n.inputs,n.outputs})for(const auto& p:ports) {
                const auto contract=p.contract.view();const auto* c=std::get_if<NumericContract>(&contract);
                exact=exact&&c&&c->dtype==DType::UInt64&&c->shape.rank==Rank::Scalar&&c->semantic==Semantic::Scalar;
            }
            return domain::ProcessingSupport{exact,{64,128,64,32}};
        };
        runtime.capabilities=c.device;
        return caps;
    }
    nc::Graph graph() const{return {"remote.numeric",{},nodes,edges};}
};
struct Link {
    Fixture& f;Transport client_io,server_io;Receipt receipt;
    control::numeric::ControlSession client,server;
    static control::numeric::SessionConfig config(Fixture& f,std::uint64_t identity,std::uint32_t mask,bool server,bool writable) {
        control::numeric::SessionConfig c;c.endpoint_authorized=true;c.device_identity="device-A";c.client_identity=identity;c.codec_mask=mask;
        c.allow_mutations=writable;c.application_capabilities=server?&f.caps:nullptr;c.advertised_capabilities=server?&f.advertised:nullptr;return c;
    }
    Link(Fixture& fixture,std::uint64_t id=1,std::uint32_t mask=3,bool writable=true,std::uint32_t server_mask=3,
         const control::numeric::SessionConfig* server_override=nullptr):f(fixture),
        client(config(f,id,mask,false,true),f.clock),server(server_override?*server_override:config(f,id,server_mask,true,writable),f.clock,&f.service) {
        client_io.other=&server_io;server_io.other=&client_io;
        client.set_reply_handler([](const nc::Message& m,void* p){static_cast<Receipt*>(p)->take(m);},&receipt);
    }
    void drain() {
        for(int i=0;i<200000&&(client.pending_bytes()||server.pending_bytes()||!client_io.input.empty()||!server_io.input.empty());++i) {
            client.poll(client_io);server.poll(server_io);
        }
    }
    void open(std::uint64_t nonce=1){assert(client.open(nonce)==Status::ok);drain();if(client.state()!=control::ControlSessionState::Ready||server.state()!=control::ControlSessionState::Ready)std::fprintf(stderr,"open: client state=%d err=%d server state=%d err=%d\n",int(client.state()),int(client.error()),int(server.state()),int(server.error()));assert(client.state()==control::ControlSessionState::Ready&&server.state()==control::ControlSessionState::Ready);}
    void request(nc::Command c,std::uint64_t request,std::uint64_t revision=0) {
        const int calls=receipt.calls;assert(client.send(c,request,revision)==Status::ok);drain();
        assert(receipt.calls==calls+1&&receipt.request==request);
    }
};
void session_cases() {
    for(const auto mask:{1u,2u,3u}) {
        Fixture f;Link a(f,1,mask),b(f,2,mask);a.open();b.open();
        assert(a.client.codec()==(mask==2?nc::CodecId::Protobuf:nc::CodecId::Cbor));
        nc::Command c;c.id=nc::CommandId::Capabilities;a.request(c,1);assert(a.receipt.rows==2);
        c.id=nc::CommandId::Prepare;c.graph=f.graph();a.request(c,2);assert(a.receipt.error==nc::Error::None);
        const auto token=a.receipt.token,revision=a.receipt.revision;assert(token);
        assert(a.receipt.resources.stack==32&&a.receipt.resources.peak==f.runtime.pending_admission->peak_bytes);
        a.request(c,2);assert(f.runtime.prepares==1&&a.receipt.token==token&&a.receipt.revision==revision);
        f.nodes[0].enabled=false;a.request(c,2);assert(a.receipt.error==nc::Error::Conflict&&f.runtime.prepares==1&&a.receipt.token==token);
        f.nodes[0].enabled=true;
        b.request(c,1,revision);assert(b.receipt.error==nc::Error::Busy);
        c={};c.id=nc::CommandId::Apply;c.graph_id="remote.numeric";c.token=token;
        b.request(c,2,revision);assert(b.receipt.error==nc::Error::Foreign);
        f.runtime.apply_status=Status::exhausted;a.request(c,3,revision);
        assert(a.receipt.error!=nc::Error::None&&a.receipt.revision==revision&&a.receipt.token==token&&!f.runtime.working&&f.runtime.pending&&f.runtime.intent);
        f.runtime.apply_status=Status::ok;a.request(c,4,revision);assert(a.receipt.error==nc::Error::None&&a.receipt.applied);
        assert(f.runtime.working&&f.runtime.applies==2&&a.receipt.graph.nodes[0].outputs[0].contract);
        assert(a.receipt.resources.stack==32&&a.receipt.resources.peak==f.runtime.working_admission->peak_bytes);
        auto applied=*f.runtime.working;const auto after=a.receipt.revision;
        c={};c.id=nc::CommandId::Stop;assert(b.client.send(c,3,after)!=Status::ok);
        c.id=nc::CommandId::Start;assert(b.client.send(c,4,after)!=Status::ok);
        assert(application::same_numeric_processing_graph(applied,*f.runtime.working)&&f.runtime.intent);
        const auto stopped=after;
        nc::Parameter ui{"position",NumericLiteral{1.0f}};
        c={};c.id=nc::CommandId::SetParameters;c.node=1;c.parameters=nc::EndpointParameters{std::span(&ui,1)};
        a.request(c,5,stopped);assert(a.receipt.error==nc::Error::Validation&&a.receipt.revision==stopped);
        assert(application::same_numeric_processing_graph(applied,*f.runtime.working)&&f.runtime.intent);
        f.source_ports[0].contract=NumericContract{f.contract};
        std::get<NumericContract>(f.source_ports[0].contract).dtype=DType::Float64;
        c={};c.id=nc::CommandId::Prepare;c.graph=f.graph();a.request(c,6,stopped);
        assert(a.receipt.error==nc::Error::Validation||a.receipt.error==nc::Error::Unsupported);
        assert(application::same_numeric_processing_graph(applied,*f.runtime.working)&&f.runtime.intent);
        f.source_ports[0].contract=f.contract;
        c={};c.id=nc::CommandId::Prepare;c.graph=f.graph();auto versions=c.graph->versions;versions.control=99;c.graph->versions=versions;
        assert(a.client.send(c,7,a.receipt.revision)!=Status::ok);assert(application::same_numeric_processing_graph(applied,*f.runtime.working));
        a.client.reset();a.server.reset();assert(a.client.identity().session==0);
    }
    Fixture f;Link read_only(f,1,3,false);read_only.open();nc::Command c;c.id=nc::CommandId::Start;
    assert(read_only.client.send(c,1)==Status::unavailable&&f.runtime.prepares==0);
    auto config=Link::config(f,2,3,true,true);f.rows[0].resources.payload=999;
    control::numeric::ControlSession bad_caps(config,f.clock,&f.service);assert(bad_caps.error()==control::ControlSessionError::InvalidConfiguration);
    config=Link::config(f,3,0,false,true);control::numeric::ControlSession zero_mask(config,f.clock);
    assert(zero_mask.error()==control::ControlSessionError::InvalidConfiguration);
    Fixture mismatch;mismatch.advertised.codec_mask=1;Link disjoint(mismatch,1,2,true,1);
    assert(disjoint.client.open(1)==Status::ok);disjoint.drain();
    assert(disjoint.client.error()==control::ControlSessionError::Unsupported&&mismatch.runtime.prepares==0);
    Fixture fragmented;Link large(fragmented);large.client_io.segment=127;large.server_io.segment=71;large.open();
    std::string text(240,'x');std::array<nc::Parameter,8> fields;std::array<std::string,8> keys;
    for(std::size_t i=0;i<fields.size();++i){keys[i]="field"+std::to_string(i);fields[i]={keys[i],std::string_view(text)};}
    std::vector<nc::Node> nodes(64,fragmented.nodes[0]);
    for(std::size_t i=0;i<nodes.size();++i){nodes[i].id=static_cast<domain::NodeId>(i);nodes[i].parameters=nc::EndpointParameters{fields};}
    c={};c.id=nc::CommandId::Prepare;c.graph=nc::Graph{"fragmented",{},nodes,{}};
    assert(large.client.send(c,1)==Status::ok);assert(large.client.pending_bytes()>65535);large.drain();
    assert(large.receipt.calls==1&&large.receipt.error==nc::Error::Limits&&fragmented.runtime.prepares==0);
    assert(large.client.state()==control::ControlSessionState::Ready&&large.server.assembly_bytes()==0);
    c={};c.id=nc::CommandId::State;large.request(c,2);assert(large.receipt.error==nc::Error::None);
    large.client_io.blocked=true;assert(large.client.send(c,3)==Status::ok);
    fragmented.clock.now+=6000;large.client.poll(large.client_io);assert(large.client.error()==control::ControlSessionError::Timeout);
}
void data_fence_cases() {
    Fixture fixture;Link link(fixture);link.open();
    std::vector<std::uint8_t> received;
    link.server.set_packet_handler([](bdsp_protocol::PacketView packet,void* context) {
        auto& out=*static_cast<std::vector<std::uint8_t>*>(context);
        assert(packet.id==bdsp_protocol::numeric_data::EventsId);out.assign(packet.payload.begin(),packet.payload.end());
    },&received);
    std::array<std::uint8_t,128> payload;for(std::size_t i=0;i<payload.size();++i)payload[i]=std::uint8_t(i);
    const auto identity=link.client.identity();
    assert(link.client.send_packet(bdsp_protocol::numeric_data::EventsId,payload)==Status::ok);
    assert(link.client.discard_outbound_data()==Status::ok&&link.client.pending_bytes()==0);
    link.drain();assert(received.empty()&&link.client.identity()==identity);
    nc::Command state;state.id=nc::CommandId::State;link.request(state,1);
    assert(link.client.send_packet(bdsp_protocol::numeric_data::EventsId,payload)==Status::ok);
    assert(link.client.poll(link.client_io)==Status::ok);
    assert(link.client.discard_outbound_data()==Status::would_block);
    link.drain();assert(received==std::vector<std::uint8_t>(payload.begin(),payload.end()));
    assert(link.client.identity()==identity&&link.client.state()==control::ControlSessionState::Ready);
    const auto calls=link.receipt.calls;
    assert(link.client.send(state,2,link.receipt.revision)==Status::ok);
    assert(link.client.discard_outbound_data()==Status::ok&&link.client.pending_bytes()>0);
    link.drain();assert(link.receipt.calls==calls+1&&link.receipt.request==2&&link.receipt.error==nc::Error::None);
}
void reconnect_nonce_cases() {
    Fixture fixture;Link link(fixture);link.open(101);
    const auto first=link.client.identity();
    link.client.reset();link.server.reset();link.open(7);
    const auto lower=link.client.identity();assert(lower.session!=first.session&&lower.epoch==first.epoch);
    link.client.reset();link.server.reset();link.open(7);
    const auto repeated=link.client.identity();assert(repeated.session!=lower.session&&repeated.epoch==lower.epoch);
    // Replay from the previous physical session remains foreign even when its
    // nonce matches. The server, not the client's nonce ordering, issues identity.
    nc::Message stale;stale.client=lower.client;stale.session=lower.session;stale.epoch=lower.epoch;stale.request=1;
    nc::Command state;state.id=nc::CommandId::State;stale.body=state;
    std::array<std::uint8_t,2048> bytes{},scratch{},packet{};nc::CborCodec codec;
    const auto encoded=codec.encode(stale,bytes,scratch,nc::desktop_limits());assert(encoded);
    const auto framed=bdsp_protocol::encode_packet(control::ControlCommandId,std::span(bytes).first(encoded.produced),packet);assert(framed);
    link.server_io.input.insert(link.server_io.input.end(),packet.begin(),packet.begin()+std::ptrdiff_t(framed.written_bytes));
    link.drain();assert(link.server.error()==control::ControlSessionError::Identity&&fixture.runtime.prepares==0);
}
void compact_storage_cases() {
    for(const auto mask:{1u,2u}) {
        Fixture fixture;
        struct Workspace {
            std::vector<std::max_align_t> bytes;bool leased=false,refuse=false;int acquisitions=0,releases=0;
        } workspace;
        // The server advertises BOTH codecs; each client selects one independently.
        auto config=Link::config(fixture,1,3,true,true);config.max_packet_bytes=512;config.read_chunk_bytes=127;
        workspace.bytes.resize(config.codec_limits.arena_bytes/sizeof(std::max_align_t)+1);
        config.staging_context=&workspace;
        config.acquire_staging=[](void* context,std::size_t requested,std::span<std::byte>& output) noexcept {
            auto& w=*static_cast<Workspace*>(context);output={};
            if(w.leased||w.refuse)return Status::would_block;
            output={reinterpret_cast<std::byte*>(w.bytes.data()),w.bytes.size()*sizeof(std::max_align_t)};
            if(output.size()<requested){output={};return Status::exhausted;}
            w.leased=true;++w.acquisitions;return Status::ok;
        };
        config.release_staging=[](void* context) noexcept {
            auto& w=*static_cast<Workspace*>(context);assert(w.leased);w.leased=false;++w.releases;
        };
        auto private_config=config;private_config.acquire_staging=nullptr;private_config.release_staging=nullptr;
        const auto shared=control::numeric::ControlSession::storage_requirements(config);
        const auto own=control::numeric::ControlSession::storage_requirements(private_config);
        assert(shared&&own&&own->persistent_bytes-shared->persistent_bytes==config.codec_limits.arena_bytes);
        assert(shared->validation_scratch_bytes==0&&shared->external_staging_bytes==config.codec_limits.arena_bytes);
        Link link(fixture,1,mask,true,3,&config);link.open();
        struct Observed { Workspace* workspace;Receipt receipt; } observed{&workspace,{}};
        link.server.set_reply_handler([](const nc::Message& message,void* context) {
            auto& o=*static_cast<Observed*>(context);assert(!o.workspace->leased);o.receipt.take(message);
        },&observed);
        nc::Command state;state.id=nc::CommandId::State;
        link.server_io.blocked=true;assert(link.client.send(state,1)==Status::ok);
        for(int i=0;i<1000&&observed.receipt.calls==0;++i){link.client.poll(link.client_io);link.server.poll(link.server_io);}
        assert(observed.receipt.calls==1&&link.server.pending_bytes());
        // Inject a complete fragmented request while TX is blocked. RX must
        // retain it until TX drains, then become encode scratch only AFTER decode.
        nc::Message message;const auto identity=link.client.identity();
        message.client=identity.client;message.session=identity.session;message.epoch=identity.epoch;message.request=2;
        std::vector<nc::Node> nodes(fixture.nodes.begin(),fixture.nodes.end());
        for(domain::NodeId id=3;id<=8;++id){nodes.push_back(fixture.nodes[0]);nodes.back().id=id;}
        nc::Command prepare;prepare.id=nc::CommandId::Prepare;
        prepare.graph=nc::Graph{"fragmented.deferred",{},nodes,fixture.edges};message.body=prepare;
        std::vector<std::uint8_t> bytes(config.codec_limits.wire_bytes),scratch(bytes.size());
        nc::CborCodec cbor;nc::ProtobufCodec protobuf;
        const auto& codec=mask==1?static_cast<const nc::Codec&>(cbor):static_cast<const nc::Codec&>(protobuf);
        const auto encoded=codec.encode(message,bytes,scratch,config.codec_limits);assert(encoded&&encoded.produced>512);
        for(std::size_t offset=0;offset<encoded.produced;) {
            const auto count=std::min<std::size_t>(512-control::ControlFragmentHeaderBytes,encoded.produced-offset);
            std::array<std::uint8_t,512> payload{};std::size_t at=0;
            auto put=[&](std::uint64_t value,unsigned width){for(unsigned i=0;i<width;++i)payload[at++]=std::uint8_t(value>>(i*8));};
            put(1,2);put(identity.session,8);put(1,8);put(encoded.produced,4);put(offset,4);
            put(control::ControlCommandId,1);put(count,2);std::copy_n(bytes.data()+offset,count,payload.data()+at);
            std::array<std::uint8_t,1024> packet{};
            const auto framed=bdsp_protocol::encode_packet(control::ControlFragmentId,std::span(payload).first(at+count),packet);assert(framed);
            link.server_io.input.insert(link.server_io.input.end(),packet.begin(),packet.begin()+std::ptrdiff_t(framed.written_bytes));offset+=count;
        }
        for(int i=0;i<10000&&!link.server_io.input.empty();++i)link.server.poll(link.server_io);
        assert(link.server.state()==control::ControlSessionState::Ready&&observed.receipt.calls==1&&fixture.runtime.prepares==0);
        link.server_io.blocked=false;
        for(int i=0;i<10000&&(observed.receipt.calls<2||link.server.pending_bytes());++i)link.server.poll(link.server_io);
        assert(observed.receipt.calls==2&&observed.receipt.request==2&&observed.receipt.error==nc::Error::None&&fixture.runtime.prepares==1);
        assert(!workspace.leased&&workspace.acquisitions==workspace.releases);
        domain::NumericProcessingGraph graph;assert(control::numeric::own_graph(fixture.graph(),config.codec_limits,graph));
        const auto envelope=control::numeric::reply_envelope_fits(graph,config.codec_limits,fixture.advertised);assert(envelope);
        auto short_limits=config.codec_limits;short_limits.wire_bytes=envelope.required_wire-1;
        assert(!control::numeric::reply_envelope_fits(graph,short_limits,fixture.advertised));
        // Refusing the shared workspace must fail before mapping/dispatch and
        // must not release a lease that was never successfully acquired.
        workspace.refuse=true;message.request=3;message.body=state;
        const auto rejected=codec.encode(message,bytes,scratch,config.codec_limits);assert(rejected);
        std::array<std::uint8_t,1024> packet{};
        const auto framed=bdsp_protocol::encode_packet(control::ControlCommandId,std::span(bytes).first(rejected.produced),packet);assert(framed);
        link.server_io.input.insert(link.server_io.input.end(),packet.begin(),packet.begin()+std::ptrdiff_t(framed.written_bytes));
        for(int i=0;i<1000&&link.server.state()==control::ControlSessionState::Ready;++i)link.server.poll(link.server_io);
        assert(link.server.error()==control::ControlSessionError::Codec&&fixture.runtime.prepares==1&&observed.receipt.calls==2);
        assert(!workspace.leased&&workspace.acquisitions==workspace.releases);
    }
}
int main(int argc,char**) {native_scenario=argc>1;mapper_cases();session_cases();data_fence_cases();reconnect_nonce_cases();compact_storage_cases();std::puts("numeric control: all10 dtype/rank BOTH codecs + negotiated sessions/replay/two clients/rejected Apply/data fence/reconnect/compact leases GREEN");}

