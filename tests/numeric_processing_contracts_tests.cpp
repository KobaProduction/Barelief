#include <domain/processing_contracts.h>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <utility>
using namespace domain;
using namespace signal_contracts;
namespace allocation_probe {
int fail_after=-1;
std::size_t allocations=0;
void* allocate(std::size_t bytes,bool nothrow) noexcept {
    ++allocations;
    if(nothrow && fail_after>=0){if(fail_after==0)return nullptr;--fail_after;}
    return std::malloc(bytes?bytes:1);
}
}
void* operator new(std::size_t bytes){auto* p=allocation_probe::allocate(bytes,false);if(!p)std::abort();return p;}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void* operator new(std::size_t bytes,const std::nothrow_t&) noexcept {return allocation_probe::allocate(bytes,true);}
void* operator new[](std::size_t bytes,const std::nothrow_t&) noexcept {return allocation_probe::allocate(bytes,true);}
void operator delete(void* p) noexcept {std::free(p);}
void operator delete[](void* p) noexcept {std::free(p);}
void operator delete(void* p,std::size_t) noexcept {std::free(p);}
void operator delete[](void* p,std::size_t) noexcept {std::free(p);}
void operator delete(void* p,const std::nothrow_t&) noexcept {std::free(p);}
void operator delete[](void* p,const std::nothrow_t&) noexcept {std::free(p);}
namespace {
int failures=0;
#define CHECK(x) do {if(!(x)){std::printf("FAIL line %d: %s\n",__LINE__,#x);++failures;}}while(false)
const FrameLimits frame_limits{16,4096,65536,4096,256,4096};
const ProcessingAdmissionLimits limits{16,32,16,16,256,32768,frame_limits};
constexpr std::array<std::uint32_t,2> channels{7,42};
NumericContract vector_contract(DType type=DType::UInt64) {
    NumericContract c;c.dtype=type;c.semantic=Semantic::MelEnergy;c.quantity=Quantity::MelEnergy;
    c.shape.rank=Rank::Vector;c.shape.axes={Axis::Band,Axis::None};c.shape.extents={ExtentBounds{4,4},ExtentBounds{1,1}};
    c.channel_order=channels;c.clock.domain="sensor.clock";c.clock.tick_seconds={1,48000};
    c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{11,1},{37,2}};return c;
}
OwnedProcessingContract owned(const DataContract& c) {
    OwnedProcessingContract out;CHECK(own_processing_contract(c,frame_limits,8192,out)==Status::ok);return out;
}
ProcessingPort port(PortId id,PortDirection d,const DataContract& c) {return {id,d,owned(c)};}
ConfiguredProcessingNode source(NodeId id,const NumericContract& c) {
    ConfiguredProcessingNode n;n.id=id;n.kind="NetworkInput";n.outputs.push_back(port(0,PortDirection::Output,c));return n;
}
ConfiguredProcessingNode sink(NodeId id,const DataContract& c,std::string kind="NetworkOutput") {
    ConfiguredProcessingNode n;n.id=id;n.kind=std::move(kind);n.inputs.push_back(port(0,PortDirection::Input,c));return n;
}
ConfiguredProcessingNode route(NodeId id,const NumericContract& c,std::string kind="Splitter") {
    ConfiguredProcessingNode n;n.id=id;n.kind=std::move(kind);n.parameters=RouteProcessingParameters{};
    n.inputs.push_back(port(0,PortDirection::Input,c));
    n.outputs.push_back(port(0,PortDirection::Output,c));n.outputs.push_back(port(1,PortDirection::Output,c));return n;
}
ProcessingDiagnostic resolve(const ConfiguredProcessingNode& n) {ResolvedProcessingNode out;return resolve_processing_node(n,limits,out);}
void owner_tests() {
    OwnedProcessingContract snapshot;
    {
        std::array<std::uint32_t,2> donor_channels{7,42};std::string donor_clock="temporary.domain";
        auto c=vector_contract();c.channel_order=donor_channels;c.clock.domain=donor_clock;
        CHECK(own_processing_contract(c,frame_limits,8192,snapshot)==Status::ok);
        donor_channels.fill(0);donor_clock.assign(200,'x');
    }
    if(snapshot) {
        const auto c=std::get<NumericContract>(snapshot.view());CHECK(c.channel_order[0]==7&&c.channel_order[1]==42);
        CHECK(c.clock.domain=="temporary.domain");CHECK(c.coordinates[0].step==Ratio(37,2));
        auto copy=snapshot;auto moved=std::move(snapshot);CHECK(!snapshot);snapshot=copy;copy={};
        CHECK(same_contract(snapshot.view(),moved.view()));CHECK(snapshot.owned_bytes()>sizeof(NumericContract));
        const auto bytes=snapshot.owned_bytes();auto old=snapshot.view();
        CHECK(own_processing_contract(old,frame_limits,bytes-1,snapshot)==Status::buffer_too_small);
        CHECK(same_contract(old,snapshot.view()));
        CHECK(own_processing_contract(old,frame_limits,bytes*2-1,snapshot)==Status::buffer_too_small);
        CHECK(own_processing_contract(old,frame_limits,bytes*2,snapshot)==Status::ok);
        auto* alias=&snapshot;snapshot=*alias;snapshot=std::move(*alias);CHECK(same_contract(old,snapshot.view()));
    }
    OwnedProcessingContract structured;
    {std::string schema="custom.schema",encoding="binary.le";CHECK(own_processing_contract(StructuredContract{schema,3,encoding,{4,64}},frame_limits,8192,structured)==Status::ok);}
    if(structured) {auto copy=structured;structured={};auto v=std::get<StructuredContract>(copy.view());CHECK(v.schema=="custom.schema"&&v.encoding=="binary.le"&&v.version==3);}
    for(const auto& dt:dtype_descriptors())for(auto rank:{Rank::Scalar,Rank::Vector,Rank::Matrix}) {
        auto c=vector_contract(dt.type);c.shape.rank=rank;c.semantic=Semantic::Numeric;
        if(rank==Rank::Scalar){c.channel_order=std::span(channels).first(1);c.shape.axes={Axis::None,Axis::None};c.shape.extents={ExtentBounds{1,1},ExtentBounds{1,1}};c.semantic=Semantic::Scalar;c.coordinates={};}
        if(rank==Rank::Matrix){c.shape.axes={Axis::Time,Axis::Band};c.shape.extents={ExtentBounds{2,8},ExtentBounds{4,4}};}
        auto o=owned(c);CHECK(o&&same_contract(c,o.view()));
    }
    auto c=vector_contract();auto retained=owned(c);auto baseline=retained;
    for(int threshold=0;threshold<3;++threshold){allocation_probe::fail_after=threshold;
        auto result=own_processing_contract(c,frame_limits,8192,retained);allocation_probe::fail_after=-1;
        CHECK(result==Status::exhausted);CHECK(same_contract(baseline.view(),retained.view()));}
    allocation_probe::fail_after=3;auto result=own_processing_contract(c,frame_limits,8192,retained);allocation_probe::fail_after=-1;CHECK(result==Status::ok);
    const auto prior_allocations=allocation_probe::allocations;
    {auto copy=retained;auto moved=std::move(copy);CHECK(same_contract(moved.view(),retained.view()));}
    CHECK(allocation_probe::allocations==prior_allocations);
}
void resolver_tests() {
    {
        auto input=vector_contract(),scalar=input;scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.subspan(1,1);
        ConfiguredProcessingNode extract;extract.id=8;extract.kind="IndexExtract";extract.inputs={{0,PortDirection::Input,owned(input)}};extract.outputs={{0,PortDirection::Output,owned(scalar)}};
        extract.parameters=TransformProcessingParameters{{{"channel_index",NumericLiteral{std::uint64_t{1}}},{"index",NumericLiteral{std::uint64_t{3}}}}};
        ResolvedProcessingNode resolved;CHECK(resolve_processing_node(extract,limits,resolved));CHECK(resolved.operation==ProcessingOperation::Extract);
        auto invalid=extract;std::get<TransformProcessingParameters>(invalid.parameters).values.back().value=NumericLiteral{std::uint64_t{4}};CHECK(!resolve_processing_node(invalid,limits,resolved));
        invalid=extract;auto wrong=scalar;wrong.dtype=DType::Float64;invalid.outputs[0].contract=owned(wrong);CHECK(!resolve_processing_node(invalid,limits,resolved));
        invalid=extract;std::get<TransformProcessingParameters>(invalid.parameters).values.push_back({"column",NumericLiteral{std::uint64_t{1}}});CHECK(!resolve_processing_node(invalid,limits,resolved));
        ConfiguredProcessingNode display;display.id=9;display.kind="ScalarView";display.inputs={{3,PortDirection::Input,owned(scalar)},{7,PortDirection::Input,owned(wrong)}};CHECK(resolve_processing_node(display,limits,resolved));
        auto multi=scalar;multi.channel_order=input.channel_order;auto retained=display.inputs[1].contract;
        CHECK(own_processing_contract(multi,frame_limits,8192,retained)==Status::incompatible_contract);
        CHECK(same_contract(retained.view(),display.inputs[1].contract.view()) && resolve_processing_node(display,limits,resolved));
        display.inputs[1].contract=owned(input);CHECK(!resolve_processing_node(display,limits,resolved));
        for(const char* kind:{"AudioView","WindowView","WindowedView","FFTView","MelView","SpectrogramView"}){display.kind=kind;display.inputs.resize(1);display.inputs[0].id=0;CHECK(!resolve_processing_node(display,limits,resolved));}
    }
    auto c=vector_contract();auto n=route(4,c);ResolvedProcessingNode r;
    CHECK(resolve_processing_node(n,limits,r));CHECK(r.role==NodeRole::Processor&&r.operation==ProcessingOperation::Splitter&&r.outputs[1].id==1);
    std::swap(n.outputs[0],n.outputs[1]);CHECK(resolve_processing_node(n,limits,r));CHECK(r.outputs[0].id==1);std::swap(n.outputs[0],n.outputs[1]);
    const auto prior=r;
    n.outputs[1].id=0;CHECK(resolve_processing_node(n,limits,r).error==ProcessingError::InvalidPort);CHECK(r.parameters==prior.parameters);
    n=route(4,c);auto changed=c;changed.dtype=DType::Float32;n.outputs[1].contract=owned(changed);
    CHECK(resolve(n).error==ProcessingError::IncompatiblePorts);
    n=route(4,c,"Switch");std::get<RouteProcessingParameters>(n.parameters).selected=2;CHECK(resolve(n).error==ProcessingError::InvalidParameters);
    n=route(4,c,"Gate");n.outputs.resize(1);std::get<RouteProcessingParameters>(n.parameters).outputs=1;CHECK(resolve(n));
    n=sink(5,c,"MelView");CHECK(resolve_processing_node(n,limits,r));CHECK(r.role==NodeRole::Sink&&r.outputs.empty());
    n.outputs.push_back(port(0,PortDirection::Output,c));CHECK(resolve(n).error==ProcessingError::InvalidPort);
    n=source(1,c);n.kind="Invented";CHECK(resolve(n).error==ProcessingError::UnknownKind);
    n=source(1,c);CHECK(resolve_processing_node(n,limits,r));CHECK(r.role==NodeRole::Source&&r.inputs.empty());
    n=source(1,c);std::get<EndpointProcessingParameters>(n.parameters).values.push_back({"channel",NumericLiteral{std::uint64_t{1}}});CHECK(resolve(n).error==ProcessingError::InvalidParameters);
    n=sink(4,c,"MelView");std::get<EndpointProcessingParameters>(n.parameters).values.push_back({"channel",NumericLiteral{std::uint64_t{1}}});CHECK(resolve(n).error==ProcessingError::InvalidParameters);
    ConfiguredProcessingNode converter;converter.id=9;converter.kind="Converter";converter.parameters=ConverterProcessingParameters{};
    converter.inputs.push_back(port(0,PortDirection::Input,c));converter.outputs.push_back(port(0,PortDirection::Output,changed));
    CHECK(resolve_processing_node(converter,limits,r));CHECK(r.operation==ProcessingOperation::Converter);
    changed.semantic=Semantic::Numeric;converter.outputs[0].contract=owned(changed);CHECK(resolve(converter));
    converter.outputs[0].contract=owned(c);auto& policy=std::get<ConverterProcessingParameters>(converter.parameters).policy;
    policy.nan_replacement=std::bit_cast<double>(std::uint64_t{0x7ff8000000001234});CHECK(resolve(converter));
    auto duplicate=converter.parameters;CHECK(same_processing_parameters(converter.parameters,duplicate));
    std::get<ConverterProcessingParameters>(duplicate).policy.nan_replacement=std::bit_cast<double>(std::uint64_t{0x7ff8000000005678});CHECK(!same_processing_parameters(converter.parameters,duplicate));
    policy.rounding=static_cast<Rounding>(77);CHECK(resolve(converter).error==ProcessingError::InvalidParameters);
}
void reduction_history_tests() {
    auto c=vector_contract();NumericContract scalar;scalar.dtype=DType::UInt64;scalar.channel_order=std::span(channels).first(1);
    scalar.quantity=c.quantity;scalar.unit=c.unit;scalar.clock=c.clock;
    ConfiguredProcessingNode n;n.id=5;n.kind="ScalarReduce";n.parameters=ReduceProcessingParameters{};
    n.inputs.push_back(port(0,PortDirection::Input,c));n.outputs.push_back(port(0,PortDirection::Output,scalar));CHECK(resolve(n));
    auto& p=std::get<ReduceProcessingParameters>(n.parameters);p.channels=ReductionChannels::BothChannels;CHECK(resolve(n));
    p.channels=ReductionChannels::SelectedChannels;p.selected_channels={42};CHECK(resolve(n));p.selected_channels={999};CHECK(resolve(n).error==ProcessingError::InvalidParameters);
    p.selected_channels={42,42};CHECK(resolve(n).error==ProcessingError::InvalidParameters);p.selected_channels={7};
    auto h=c;h.semantic=Semantic::History;h.history_element=c.semantic;h.shape.rank=Rank::Matrix;
    h.shape.axes={Axis::Time,Axis::Band};h.shape.extents={ExtentBounds{0,8},c.shape.extents[0]};h.coordinates={AxisCoordinates{},c.coordinates[0]};
    n.inputs[0].contract=owned(h);CHECK(resolve(n)); // WholeValue Matrix reduction is explicit and structurally supported.
    p.rows=ReductionRows::LatestHistoryRow;CHECK(resolve(n));
    for(auto op:{NumericReduction::SignedMean,NumericReduction::RMS}){
        p.operation=op;CHECK(resolve(n).error==ProcessingError::InvalidContract);
        auto result=scalar;result.dtype=DType::Float64;n.outputs[0].contract=owned(result);CHECK(resolve(n));n.outputs[0].contract=owned(scalar);
    }
    p.operation=NumericReduction::MaxAbs;
    ConfiguredProcessingNode acc;acc.id=8;acc.kind="Spectrogram";acc.parameters=HistoryProcessingParameters{8};
    acc.inputs.push_back(port(0,PortDirection::Input,c));acc.outputs.push_back(port(history_output,PortDirection::Output,h));
    auto count=scalar;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
    acc.outputs.push_back(port(filled_count_output,PortDirection::Output,count));CHECK(resolve(acc));
    auto variable=c;variable.shape.extents[0].minimum=1;acc.inputs[0].contract=owned(variable);
    auto variable_history=h;variable_history.shape.extents[1]=variable.shape.extents[0];acc.outputs[0].contract=owned(variable_history);
    CHECK(resolve(acc).error==ProcessingError::InvalidContract); // runtime has always required fixed row geometry
    acc.inputs[0].contract=owned(c);
    auto regular=h;regular.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,60}};
    acc.outputs[0].contract=owned(regular);CHECK(resolve(acc).error==ProcessingError::InvalidContract);
    auto explicit_time=h;explicit_time.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    acc.outputs[0].contract=owned(explicit_time);CHECK(resolve(acc));acc.outputs[0].contract=owned(h);
    count.dtype=DType::UInt32;acc.outputs[1].contract=owned(count);CHECK(resolve(acc).error==ProcessingError::InvalidContract);
    count.dtype=DType::UInt64;acc.outputs[1].contract=owned(count);std::get<HistoryProcessingParameters>(acc.parameters).capacity=9;CHECK(resolve(acc).error==ProcessingError::IncompatiblePorts);
    StructuredContract packet{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};
    ConfiguredProcessingNode encoder;encoder.id=6;encoder.kind="WLEDEncode";encoder.parameters=TransformProcessingParameters{};
    encoder.inputs.push_back(port(0,PortDirection::Input,c));encoder.outputs.push_back(port(0,PortDirection::Output,packet));CHECK(resolve(encoder));
    auto receiver=sink(7,packet,"WLEDOutput");CHECK(resolve(receiver));CHECK(processing_ports_compatible(encoder.outputs[0],receiver.inputs[0]));
    auto bad=packet;bad.version=3;receiver.inputs[0].contract=owned(bad);CHECK(!processing_ports_compatible(encoder.outputs[0],receiver.inputs[0]));
    encoder.outputs[0].contract=owned(c);CHECK(resolve(encoder).error==ProcessingError::InvalidContract);
}
struct QueryState {std::size_t calls=0;bool allow=true;std::size_t bytes=5;};
ProcessingSupport query(const void* context,const ResolvedProcessingNode& n,const ProcessingResourceProfile&) noexcept {
    auto& s=*const_cast<QueryState*>(static_cast<const QueryState*>(context));++s.calls;
    bool ok=s.allow;
    // Deliberately non-Cartesian combination: only UInt64 Mel, not arbitrary UInt64 or Mel.
    for(const auto& p:n.outputs){auto v=p.contract.view();if(const auto* c=std::get_if<NumericContract>(&v))ok=ok&&c->dtype==DType::UInt64&&c->semantic==Semantic::MelEnergy;}
    return {ok,{s.bytes,0,0,2}};
}
void graph_tests() {
    auto c=vector_contract();NumericProcessingGraph g;g.graph_id="actual.graph";g.nodes={source(1,c),route(2,c),sink(3,c)};g.edges={{1,2,0,0},{2,3,1,0}};
    QueryState q;NumericDeviceCapabilities caps{&q,query,{{100,100,100,20},11,65536}};
    std::array<std::size_t,32> work{};ProcessingGraphAdmission out{999,{7,8,9,10},123};
    CHECK(admit_processing_graph(g,limits,caps,work,out));CHECK(out.node_count==3&&out.resources.plan_bytes==15&&out.resources.stack_bytes==2);CHECK(q.calls==3);
    const auto frozen=out;
    g.edges.push_back({1,3,0,0});CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::MultipleInputs);CHECK(out.node_count==frozen.node_count&&out.peak_bytes==frozen.peak_bytes);g.edges.pop_back();
    g.edges[1].output_port=2;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::MissingEndpoint);g.edges[1].output_port=1;
    auto changed=c;changed.dtype=DType::Float64;g.nodes[2].inputs[0].contract=owned(changed);CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::UnsupportedCombination);g.nodes[2]=sink(3,c);
    q.allow=false;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::UnsupportedCombination);q.allow=true;
    caps.profile.maximum.plan_bytes=14;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::ResourceLimit);caps.profile.maximum.plan_bytes=100;
    caps.profile.max_peak_bytes=1;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::ResourceLimit);caps.profile.max_peak_bytes=65536;
    CHECK(admit_processing_graph(g,limits,caps,std::span(work).first(5),out).error==ProcessingError::InsufficientWorkspace);
    auto tiny=limits;tiny.max_nodes=2;CHECK(admit_processing_graph(g,tiny,caps,work,out).error==ProcessingError::Limit);
    g.versions.processing=1;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::UnsupportedVersion);g.versions.processing=2;
    auto old=g.versions;old.data=3;CHECK(processing_versions_supported(old)==Status::ok);CHECK(processing_versions_supported(g.versions)==Status::ok);
    g.nodes[2]=route(3,c);g.edges.push_back({3,2,0,0});g.edges.erase(g.edges.begin());CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::Cycle);
    g.nodes[2].id=2;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::InvalidNode);
    CHECK(out.node_count==frozen.node_count&&out.peak_bytes==frozen.peak_bytes);
}
void admission_boundary_tests() {
    auto c=vector_contract();NumericProcessingGraph g;g.graph_id="budget.graph";g.nodes={source(1,c),sink(2,c)};g.edges={{1,2,0,0}};
    QueryState q;NumericDeviceCapabilities caps{&q,query,{{100,100,100,20},11,65536}};
    std::array<std::size_t,32> work{};ProcessingGraphAdmission out;
    CHECK(admit_processing_graph(g,limits,caps,work,out));auto before=out;
    // Reserved DTO memory is real retained memory even when the logical draft is short.
    g.nodes[0].outputs.reserve(3000);CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::Limit);
    CHECK(out.peak_bytes==before.peak_bytes);
    auto ordinary=c;ordinary.semantic=Semantic::Window;ordinary.quantity=Quantity::Amplitude;ordinary.shape.axes[0]=Axis::Sample;
    ConfiguredProcessingNode n;n.id=10;n.kind="WindowFunction";n.parameters=TransformProcessingParameters{};
    n.inputs.push_back(port(0,PortDirection::Input,ordinary));n.outputs.push_back(port(0,PortDirection::Output,c));
    CHECK(resolve(n).error==ProcessingError::IncompatiblePorts);
    ordinary.semantic=Semantic::Windowed;n.outputs[0].contract=owned(ordinary);CHECK(resolve(n));
    ResolvedProcessingNode resolved;CHECK(resolve_processing_node(n,limits,resolved));ProcessingParameterView value;
    CHECK(resolve_processing_parameter(resolved,"function",value)==Status::ok);CHECK(std::get<std::string_view>(value)=="hann");
    std::get<TransformProcessingParameters>(n.parameters).values.push_back({"function",std::string("blackman")});
    CHECK(resolve_processing_node(n,limits,resolved));CHECK(resolve_processing_parameter(resolved,"function",value)==Status::ok);CHECK(std::get<std::string_view>(value)=="blackman");
    CHECK(resolve_processing_parameter(resolved,"channel",value)==Status::invalid_argument);CHECK(std::get<std::string_view>(value)=="blackman");
    auto exact=ConverterProcessingParameters{};exact.policy.scaling.gain.numerator=std::numeric_limits<std::uint64_t>::max();
    auto other=exact;CHECK(same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
    other.policy.scaling.gain.numerator=std::uint64_t{9007199254740993};CHECK(!same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
    exact.policy.nan_replacement=-0.0;other=exact;other.policy.nan_replacement=0.0;CHECK(!same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
    caps.profile.maximum.plan_bytes=std::numeric_limits<std::size_t>::max();caps.profile.max_peak_bytes=std::numeric_limits<std::size_t>::max();
    g.nodes[0]=source(1,c);q.bytes=std::numeric_limits<std::size_t>::max();
    CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::ResourceLimit);
    q.bytes=5;CHECK(admit_processing_graph(g,limits,caps,work,out));const auto admitted_peak=out.peak_bytes;
    caps.profile.max_peak_bytes=admitted_peak-1;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::ResourceLimit);
    caps.profile.max_peak_bytes=admitted_peak;
    const auto allocations=allocation_probe::allocations;CHECK(admit_processing_graph(g,limits,caps,work,out));CHECK(allocation_probe::allocations==allocations);
    caps.query=nullptr;CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::UnsupportedCombination);caps.query=query;
    auto unsupported=c;unsupported.dtype=DType::Float64;g.nodes={source(1,unsupported),sink(2,unsupported)};
    CHECK(admit_processing_graph(g,limits,caps,work,out).error==ProcessingError::UnsupportedCombination);
    // All dtype dimensions must match exactly; advertised support is separate.
    for(const auto& dt:dtype_descriptors()){
        auto x=vector_contract(dt.type);auto output=port(0,PortDirection::Output,x);auto input=port(0,PortDirection::Input,x);CHECK(processing_ports_compatible(output,input));
        auto other=x;other.dtype=dt.type==DType::UInt64?DType::Float64:DType::UInt64;input.contract=owned(other);CHECK(!processing_ports_compatible(output,input));
        other=x;other.clock.sample_rate=1;input.contract=owned(other);CHECK(!processing_ports_compatible(output,input));
        other=x;other.representation.layout=ChannelLayout::Interleaved;input.contract=owned(other);CHECK(!processing_ports_compatible(output,input));
    }
}
void independent_findings_repairs() {
    QueryState q;NumericDeviceCapabilities device{&q,
        [](const void* context,const ResolvedProcessingNode&,const ProcessingResourceProfile&) noexcept {
            auto& state=*const_cast<QueryState*>(static_cast<const QueryState*>(context));++state.calls;
            return ProcessingSupport{state.allow,{1,2,3,4}};
        },{{100,100,100,100},10,65536}};
    std::array<std::size_t,32> work{};ProcessingGraphAdmission admission{99,{},123};
    auto pcm=vector_contract(DType::Float32);pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;pcm.shape.axes[0]=Axis::Sample;pcm.coordinates={};
    NumericProcessingGraph g;g.graph_id="local.processing";g.nodes={source(1,pcm)};
    CHECK(admit_processing_graph(g,limits,device,work,admission));
    for(auto edition:{0u,1u,99u})for(int axis=0;axis<3;++axis){g.versions={};
        if(axis==0)g.versions.document=edition;
        if(axis==1)g.versions.control=edition;
        if(axis==2)g.versions.data=edition;
        CHECK(admit_processing_graph(g,limits,device,work,admission));}
    for(int axis=0;axis<2;++axis){g.versions={};if(axis==0)g.versions.processing=1;else g.versions.graph=2;
        CHECK(admit_processing_graph(g,limits,device,work,admission).error==ProcessingError::UnsupportedVersion);}
    for(auto boundary:{ProcessingBoundary::Document,ProcessingBoundary::Control,ProcessingBoundary::Data}){
        const auto current=boundary==ProcessingBoundary::Data?4u:2u;
        CHECK(processing_boundary_version_supported(boundary,current)==Status::ok);
        for(auto edition:{0u,1u,99u})CHECK(processing_boundary_version_supported(boundary,edition)==Status::unsupported_version);
    }
    CHECK(processing_boundary_version_supported(static_cast<ProcessingBoundary>(99),2)==Status::invalid_argument);
    g.versions={};
    auto scalar=pcm;scalar.shape={};scalar.channel_order=std::span(channels).first(1);scalar.clock.sample_rate=48000;
    auto window=pcm;window.semantic=Semantic::Window;window.shape.extents[0]={512,512};window.channel_order=scalar.channel_order;window.clock=scalar.clock;
    ConfiguredProcessingNode accumulator;accumulator.id=2;accumulator.kind="Window";
    accumulator.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{512}}}}};
    accumulator.inputs={port(0,PortDirection::Input,scalar)};accumulator.outputs={port(0,PortDirection::Output,window)};
    CHECK(resolve(accumulator));g.nodes={accumulator};CHECK(admit_processing_graph(g,limits,device,work,admission));
    q.allow=false;const auto retained=admission;CHECK(admit_processing_graph(g,limits,device,work,admission).error==ProcessingError::UnsupportedCombination);
    CHECK(admission.node_count==retained.node_count&&admission.peak_bytes==retained.peak_bytes);q.allow=true;
    accumulator.kind="StereoFFT";accumulator.parameters=TransformProcessingParameters{};CHECK(resolve(accumulator).error==ProcessingError::IncompatiblePorts);
    accumulator.kind="Window";scalar.semantic=Semantic::Scalar;accumulator.inputs[0].contract=owned(scalar);CHECK(resolve(accumulator));
    auto signal_pcm=pcm;signal_pcm.clock.sample_rate=48000;
    auto signal=source(3,signal_pcm);signal.kind="TestSignal";
    auto& fields=std::get<EndpointProcessingParameters>(signal.parameters).values;
    fields={{"minimum_hz",NumericLiteral{1000.0}},{"maximum_hz",NumericLiteral{100.0}}};CHECK(resolve(signal).error==ProcessingError::InvalidParameters);
    fields={{"maximum_hz",NumericLiteral{30000.0}}};CHECK(resolve(signal).error==ProcessingError::InvalidParameters);
    g.nodes={signal};auto calls=q.calls;CHECK(admit_processing_graph(g,limits,device,work,admission).error==ProcessingError::InvalidParameters);CHECK(q.calls==calls);
    fields.clear();const auto absent=signal.parameters;CHECK(resolve(signal));CHECK(same_processing_parameters(absent,signal.parameters));
    // Nyquist equality refuses; a representable value immediately below it admits.
    fields={{"maximum_hz",NumericLiteral{24000.0}},{"sweep_depth",NumericLiteral{0.0}}};CHECK(resolve(signal).error==ProcessingError::InvalidParameters);
    fields[0].value=NumericLiteral{23999.0};CHECK(resolve(signal));
    fields={{"minimum_hz",NumericLiteral{1000.0}},{"maximum_hz",NumericLiteral{1000.0}}};CHECK(resolve(signal).error==ProcessingError::InvalidParameters);
    fields={{"sample_rate",NumericLiteral{std::uint64_t{8000}}}};CHECK(resolve(signal).error==ProcessingError::InvalidParameters);
    fields={{"maximum_hz",NumericLiteral{18000.0}},{"sample_rate",NumericLiteral{std::uint64_t{48000}}},{"sweep_depth",NumericLiteral{0.18}}};
    CHECK(resolve(signal));const auto explicit_values=signal.parameters;CHECK(resolve(signal));CHECK(same_processing_parameters(explicit_values,signal.parameters));
    for(auto kind:{"Splitter","Switch"})for(std::uint16_t count:{1,2,8,9}){
        auto n=route(4,pcm,kind);n.outputs.clear();n.parameters=RouteProcessingParameters{count,std::uint16_t(count-1),true};
        for(PortId id=0;id<count;++id)n.outputs.push_back(port(id,PortDirection::Output,pcm));
        if(count==2||count==8)CHECK(resolve(n));else CHECK(resolve(n).error==ProcessingError::InvalidParameters);
    }
}
}
static void lowering_interval_regressions(){
 auto input=vector_contract();input.dtype=DType::Float64;auto output=input;output.dtype=DType::Int16;
 NumericProcessingGraph g;g.graph_id="original.edge.index";g.nodes={source(1,input),sink(2,output),sink(3,output)};
 g.edges={{1,3,0,0},{1,2,0,0}};g.edges[1].conversion=255;
 NumericProcessingGraph untouched;untouched.graph_id="unchanged";
 const auto diagnostic=lower_dtype_edges(g,limits,untouched);CHECK(!diagnostic&&diagnostic.edge==1&&untouched.graph_id=="unchanged"&&untouched.nodes.empty());
 CHECK(valid_execution_interval({1,60})&&valid_execution_interval({1,1000})&&valid_execution_interval({60,1})&&valid_execution_interval({7,333})&&valid_execution_interval({INT64_MAX,UINT64_MAX}));
 for(const Ratio ratio:{Ratio{0,1},Ratio{-1,1},Ratio{1,0},Ratio{2,120},Ratio{1,1001},Ratio{61,1},Ratio{INT64_MAX,1}})CHECK(!valid_execution_interval(ratio));
 auto oversized=g;oversized.nodes.front().kind.assign(limits.max_owned_metadata_bytes,'x');const auto allocations_before=allocation_probe::allocations;
 CHECK(!lower_dtype_edges(oversized,limits,untouched)&&allocation_probe::allocations==allocations_before&&untouched.graph_id=="unchanged");
}
int main(){lowering_interval_regressions();owner_tests();resolver_tests();reduction_history_tests();graph_tests();admission_boundary_tests();independent_findings_repairs();std::printf("numeric processing contract failures=%d\n",failures);return failures?1:0;}
