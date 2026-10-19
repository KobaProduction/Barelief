#include <domain/processing_contracts.h>
#include <domain/graph/node_factory.h>
#include <signal_contracts/spectral_calibration.h>
#include "numeric_test_builders.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <source_location>
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
void expect(bool yes,std::source_location at=std::source_location::current()) {
    if(!yes){std::printf("FAIL %s:%u\n",at.file_name(),at.line());++failures;}
}
void expect(const ProcessingDiagnostic& result,std::source_location at=std::source_location::current()) {
    expect(static_cast<bool>(result),at);
}
void fixture_check(bool yes,const char* reason) {if(!yes){std::printf("FAIL fixture: %s\n",reason);++failures;}}
const FrameLimits frame_limits{16,4096,65536,4096,256,4096};
const ProcessingAdmissionLimits limits{16,32,16,16,256,32768,frame_limits};
void check_projection_grid(const NumericContract& input,std::uint32_t rate) {
    const Ratio expected=rate==48000?Ratio{375,4}:Ratio{11025,128};
    expect(input.clock.sample_rate==rate && input.shape.extents[0]==(ExtentBounds{257,257}));
    expect(input.coordinates[0].origin==(Ratio{0,1}));expect(input.coordinates[0].step==expected);
}
void projection_factory_at_rate(std::uint32_t rate) {
    ConfiguredProcessingNode node;
    const auto result=make_processing_node("SpectrumProjection",5,limits,node,{rate,512,256,400});
    expect(result);if(!result)return;
    check_projection_grid(std::get<NumericContract>(node.inputs[0].contract.view()),rate);
    expect(derive_processing_node_contracts(node,limits));
}
void projection_factory_grid_regression() {
    for(const auto rate:{48000u,44100u})projection_factory_at_rate(rate);
}
void check_switch_outputs(const ConfiguredProcessingNode& node,const DataContract& input) {
    for(const auto& output:node.outputs)expect(same_physical_contract(output.contract.view(),input));
}
void reconfigure_factory_switch(ConfiguredProcessingNode& node) {
    const auto retained=node.inputs[0].contract;
    auto data=retained.view();auto& input=std::get<NumericContract>(data);
    input.clock.sample_rate=44100;input.clock.tick_seconds={1,44100};input.shape.extents[0]={128,128};
    const auto parameters=node.parameters;
    expect(configure_processing_input(node,data,limits));
    expect(same_processing_parameters(parameters,node.parameters));
    check_switch_outputs(node,data);
}
void switch_factory_contract_regression() {
    ConfiguredProcessingNode node;const auto result=make_processing_node("Switch",200,limits,node);
    expect(result);if(!result)return;
    check_switch_outputs(node,node.inputs[0].contract.view());
    reconfigure_factory_switch(node);
}
void parameter_map_coefficients(const ProcessingParameters& original) {
    auto reordered=original;auto& fields=std::get<TransformProcessingParameters>(reordered).values;
    std::swap(fields[0],fields[1]);expect(same_processing_parameters(original,reordered));
    std::get<NumericArray>(fields[1].value).values[1]=3;
    expect(!same_processing_parameters(original,reordered));
}
void parameter_map_duplicates(const ProcessingParameters& original) {
    auto duplicate=original;auto& fields=std::get<TransformProcessingParameters>(duplicate).values;
    fields[1]=fields[0];expect(!same_processing_parameters(original,duplicate));
    expect(!same_processing_parameters(duplicate,original));
}
void parameter_map_identity_regression() {
    const ProcessingParameters parameters=TransformProcessingParameters{{{"values",NumericArray{{1,2}}},{"size",NumericLiteral{std::uint64_t{2}}}}};
    parameter_map_coefficients(parameters);parameter_map_duplicates(parameters);
}
constexpr std::array<std::uint32_t,2> channels{7,42};
NumericContract vector_contract(DType type=DType::UInt64) {
    NumericContract c;c.dtype=type;c.semantic=Semantic::MelEnergy;c.quantity=Quantity::MelEnergy;
    c.shape.rank=Rank::Vector;c.shape.axes={Axis::Band,Axis::None};c.shape.extents={ExtentBounds{4,4},ExtentBounds{1,1}};
    c.channel_order=std::span(channels).first(1);c.clock.domain="sensor.clock";c.clock.tick_seconds={1,48000};
    c.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{11,1},{37,2}};return c;
}
OwnedProcessingContract owned(const DataContract& c) {
    return numeric_test_builders::contract(c,frame_limits,8192,fixture_check,"own descriptor contract");
}
ProcessingPort port(PortId id,PortDirection d,const DataContract& c) {return numeric_test_builders::port(id,d,owned(c));}
ConfiguredProcessingNode source(NodeId id,const NumericContract& c) {
    return numeric_test_builders::node(id,"NetworkInput",EndpointProcessingParameters{},
        {},{port(0,PortDirection::Output,c)});
}
ConfiguredProcessingNode sink(NodeId id,const DataContract& c,std::string kind="NetworkOutput") {
    return numeric_test_builders::node(id,std::move(kind),EndpointProcessingParameters{},
        {port(0,PortDirection::Input,c)},{});
}
ConfiguredProcessingNode route(NodeId id,const NumericContract& c,std::string kind="Splitter") {
    return numeric_test_builders::node(id,std::move(kind),RouteProcessingParameters{},
        {port(0,PortDirection::Input,c)},
        {port(0,PortDirection::Output,c),port(1,PortDirection::Output,c)});
}
ProcessingDiagnostic resolve(const ConfiguredProcessingNode& n) {ResolvedProcessingNode out;return resolve_processing_node(n,limits,out);}
OwnedProcessingContract temporary_numeric_owner() {
    OwnedProcessingContract snapshot;
    std::array<std::uint32_t,2> donor_channels{7,42};std::string donor_clock="temporary.domain";
    auto contract=vector_contract();contract.channel_order=donor_channels;contract.clock.domain=donor_clock;
    expect(own_processing_contract(contract,frame_limits,8192,snapshot)==Status::ok);
    donor_channels.fill(0);donor_clock.assign(200,'x');return snapshot;
}
void check_numeric_owner(const OwnedProcessingContract& snapshot) {
    const auto contract=std::get<NumericContract>(snapshot.view());
    expect(contract.channel_order[0]==7&&contract.channel_order[1]==42);
    expect(contract.clock.domain=="temporary.domain");
    expect(contract.coordinates[0].step==Ratio(37,2));
}
void check_owner_copy_move(OwnedProcessingContract& snapshot) {
    auto copy=snapshot;auto moved=std::move(snapshot);expect(!snapshot);
    snapshot=copy;copy={};expect(same_contract(snapshot.view(),moved.view()));
    expect(snapshot.owned_bytes()>sizeof(NumericContract));
}
void check_owner_self_assignment(OwnedProcessingContract& snapshot,const DataContract& original,std::size_t bytes) {
    expect(own_processing_contract(original,frame_limits,bytes*2,snapshot)==Status::ok);
    auto* alias=&snapshot;snapshot=*alias;snapshot=std::move(*alias);
    expect(same_contract(original,snapshot.view()));
}
void check_owner_budget(OwnedProcessingContract& snapshot) {
    const auto retained=snapshot;const auto original=retained.view();const auto bytes=snapshot.owned_bytes();
    expect(own_processing_contract(original,frame_limits,bytes-1,snapshot)==Status::buffer_too_small);
    expect(same_contract(original,snapshot.view()));
    expect(own_processing_contract(original,frame_limits,bytes*2-1,snapshot)==Status::buffer_too_small);
    check_owner_self_assignment(snapshot,original,bytes);
}
void structured_owner_lifetime() {
    OwnedProcessingContract owner;
    {std::string schema="custom.schema",encoding="binary.le";
     expect(own_processing_contract(StructuredContract{schema,3,encoding,{4,64}},frame_limits,8192,owner)==Status::ok);}
    if(!owner)return;
    auto copy=owner;owner={};const auto contract=std::get<StructuredContract>(copy.view());
    expect(contract.schema=="custom.schema"&&contract.encoding=="binary.le"&&contract.version==3);
}
NumericContract ranked_owner_contract(DType type,Rank rank) {
    auto contract=vector_contract(type);contract.shape.rank=rank;contract.semantic=Semantic::Numeric;
    if(rank==Rank::Scalar) {
        contract.channel_order=std::span(channels).first(1);contract.shape.axes={Axis::None,Axis::None};
        contract.shape.extents={ExtentBounds{1,1},ExtentBounds{1,1}};contract.semantic=Semantic::Scalar;contract.coordinates={};
    }
    if(rank==Rank::Matrix){contract.shape.axes={Axis::Time,Axis::Band};contract.shape.extents={ExtentBounds{2,8},ExtentBounds{4,4}};}
    return contract;
}
void dtype_rank_ownership() {
    for(const auto& descriptor:dtype_descriptors())for(auto rank:{Rank::Scalar,Rank::Vector,Rank::Matrix}) {
        const auto contract=ranked_owner_contract(descriptor.type,rank);const auto owner=owned(contract);
        expect(owner&&same_contract(contract,owner.view()));
    }
}
void failed_owner_allocation(const NumericContract& contract,OwnedProcessingContract& retained,
                             const OwnedProcessingContract& baseline,int threshold) {
    allocation_probe::fail_after=threshold;
    const auto result=own_processing_contract(contract,frame_limits,8192,retained);allocation_probe::fail_after=-1;
    expect(result==Status::exhausted);expect(same_contract(baseline.view(),retained.view()));
}
void owner_handle_copy_noalloc(const OwnedProcessingContract& retained) {
    const auto before=allocation_probe::allocations;
    {auto copy=retained;auto moved=std::move(copy);expect(same_contract(moved.view(),retained.view()));}
    expect(allocation_probe::allocations==before);
}
void owner_allocation_failure_atomicity() {
    const auto contract=vector_contract();auto retained=owned(contract);const auto baseline=retained;
    for(int threshold=0;threshold<3;++threshold)failed_owner_allocation(contract,retained,baseline,threshold);
    allocation_probe::fail_after=3;const auto result=own_processing_contract(contract,frame_limits,8192,retained);
    allocation_probe::fail_after=-1;expect(result==Status::ok);owner_handle_copy_noalloc(retained);
}
void owner_tests() {
    auto snapshot=temporary_numeric_owner();
    if(snapshot){check_numeric_owner(snapshot);check_owner_copy_move(snapshot);check_owner_budget(snapshot);}
    structured_owner_lifetime();dtype_rank_ownership();owner_allocation_failure_atomicity();
}
struct ExtractorFixture {
    NumericContract input=vector_contract(),scalar=input;
    ConfiguredProcessingNode node;
    ExtractorFixture() {
        scalar.shape={};scalar.coordinates={};scalar.channel_order=input.channel_order.first(1);
        node.id=8;node.kind="IndexExtract";node.inputs={{0,PortDirection::Input,owned(input)}};
        node.outputs={{0,PortDirection::Output,owned(scalar)}};
        node.parameters=TransformProcessingParameters{{{"channel_index",NumericLiteral{std::uint64_t{0}}},{"index",NumericLiteral{std::uint64_t{3}}}}};
    }
};
void valid_extractor(const ExtractorFixture& fixture) {
    ResolvedProcessingNode resolved;
    expect(resolve_processing_node(fixture.node,limits,resolved));expect(resolved.operation==ProcessingOperation::Extract);
}
void reject_extractor_index(const ExtractorFixture& fixture) {
    auto invalid=fixture.node;ResolvedProcessingNode resolved;
    std::get<TransformProcessingParameters>(invalid.parameters).values.back().value=NumericLiteral{std::uint64_t{4}};
    expect(!resolve_processing_node(invalid,limits,resolved));
}
void reject_extractor_contract(const ExtractorFixture& fixture) {
    auto invalid=fixture.node;auto wrong=fixture.scalar;wrong.dtype=DType::Float64;
    invalid.outputs[0].contract=owned(wrong);ResolvedProcessingNode resolved;
    expect(!resolve_processing_node(invalid,limits,resolved));
}
void reject_extractor_parameter(const ExtractorFixture& fixture) {
    auto invalid=fixture.node;ResolvedProcessingNode resolved;
    std::get<TransformProcessingParameters>(invalid.parameters).values.push_back({"column",NumericLiteral{std::uint64_t{1}}});
    expect(!resolve_processing_node(invalid,limits,resolved));
}
ConfiguredProcessingNode scalar_display(const ExtractorFixture& fixture) {
    auto alternate=fixture.scalar;alternate.dtype=DType::Float64;
    ConfiguredProcessingNode display;display.id=9;display.kind="ScalarView";
    display.inputs={{3,PortDirection::Input,owned(fixture.scalar)},{7,PortDirection::Input,owned(alternate)}};
    return display;
}
void display_multiplane_refusal(ConfiguredProcessingNode& display,const NumericContract& scalar) {
    auto multi=scalar;multi.channel_order=channels;auto retained=display.inputs[1].contract;
    expect(own_processing_contract(multi,frame_limits,8192,retained)==Status::incompatible_contract);
    ResolvedProcessingNode resolved;
    expect(same_contract(retained.view(),display.inputs[1].contract.view())&&resolve_processing_node(display,limits,resolved));
}
void display_rank_contracts(ConfiguredProcessingNode& display,const NumericContract& vector) {
    ResolvedProcessingNode resolved;display.inputs[1].contract=owned(vector);
    expect(!resolve_processing_node(display,limits,resolved));
    display.kind="FFTView";display.inputs.resize(1);display.inputs[0].id=0;
    expect(resolve_processing_node(display,limits,resolved));
}
void reject_specialized_scalar_displays(ConfiguredProcessingNode& display) {
    ResolvedProcessingNode resolved;
    for(const char* kind:{"AudioView","WindowView","WindowedView","MelView","SpectrogramView"}) {
        display.kind=kind;expect(!resolve_processing_node(display,limits,resolved));
    }
}
void extractor_and_display_tests() {
    const ExtractorFixture fixture;valid_extractor(fixture);reject_extractor_index(fixture);
    reject_extractor_contract(fixture);reject_extractor_parameter(fixture);
    auto display=scalar_display(fixture);ResolvedProcessingNode resolved;
    expect(resolve_processing_node(display,limits,resolved));display_multiplane_refusal(display,fixture.scalar);
    display_rank_contracts(display,fixture.input);reject_specialized_scalar_displays(display);
}
void reordered_route(ConfiguredProcessingNode& node) {
    ResolvedProcessingNode resolved;std::swap(node.outputs[0],node.outputs[1]);
    expect(resolve_processing_node(node,limits,resolved));expect(resolved.outputs[0].id==1);
    std::swap(node.outputs[0],node.outputs[1]);
}
void valid_routing_node() {
    auto node=route(4,vector_contract());ResolvedProcessingNode resolved;
    expect(resolve_processing_node(node,limits,resolved));
    expect(resolved.role==NodeRole::Processor&&resolved.operation==ProcessingOperation::Splitter&&resolved.outputs[1].id==1);
    reordered_route(node);
}
void invalid_route_preserves_resolution() {
    auto node=route(4,vector_contract());ResolvedProcessingNode resolved;
    expect(resolve_processing_node(node,limits,resolved));const auto prior=resolved;
    node.outputs[1].id=0;expect(resolve_processing_node(node,limits,resolved).error==ProcessingError::InvalidPort);
    expect(resolved.parameters==prior.parameters);
}
void incompatible_routing_output() {
    const auto contract=vector_contract();auto node=route(4,contract);auto changed=contract;changed.dtype=DType::Float32;
    node.outputs[1].contract=owned(changed);expect(resolve(node).error==ProcessingError::IncompatiblePorts);
}
void routing_selection_limits() {
    const auto contract=vector_contract();auto node=route(4,contract,"Switch");
    std::get<RouteProcessingParameters>(node.parameters).selected=2;
    expect(resolve(node).error==ProcessingError::InvalidParameters);
    node=route(4,contract,"Gate");node.outputs.resize(1);
    std::get<RouteProcessingParameters>(node.parameters).outputs=1;expect(resolve(node));
}
void sink_port_rules() {
    const auto contract=vector_contract();auto node=sink(5,contract,"MelView");ResolvedProcessingNode resolved;
    expect(resolve_processing_node(node,limits,resolved));expect(resolved.role==NodeRole::Sink&&resolved.outputs.empty());
    node.outputs.push_back(port(0,PortDirection::Output,contract));expect(resolve(node).error==ProcessingError::InvalidPort);
}
void source_role_rules() {
    const auto contract=vector_contract();auto node=source(1,contract);node.kind="Invented";
    expect(resolve(node).error==ProcessingError::UnknownKind);
    node=source(1,contract);ResolvedProcessingNode resolved;
    expect(resolve_processing_node(node,limits,resolved));expect(resolved.role==NodeRole::Source&&resolved.inputs.empty());
}
void endpoint_parameter_rules() {
    const auto contract=vector_contract();auto node=source(1,contract);
    std::get<EndpointProcessingParameters>(node.parameters).values.push_back({"channel",NumericLiteral{std::uint64_t{1}}});
    expect(resolve(node).error==ProcessingError::InvalidParameters);
    node=sink(4,contract,"MelView");
    std::get<EndpointProcessingParameters>(node.parameters).values.push_back({"channel",NumericLiteral{std::uint64_t{1}}});
    expect(resolve(node).error==ProcessingError::InvalidParameters);
}
ConfiguredProcessingNode converter_fixture(const NumericContract& input) {
    auto changed=input;changed.dtype=DType::Float32;
    ConfiguredProcessingNode node;node.id=9;node.kind="Converter";node.parameters=ConverterProcessingParameters{};
    node.inputs.push_back(port(0,PortDirection::Input,input));node.outputs.push_back(port(0,PortDirection::Output,changed));return node;
}
void converter_contract_rules(ConfiguredProcessingNode& node) {
    ResolvedProcessingNode resolved;expect(resolve_processing_node(node,limits,resolved));
    expect(resolved.operation==ProcessingOperation::Converter);
    const auto owner=node.outputs[0].contract;auto output=owner.view();
    std::get<NumericContract>(output).semantic=Semantic::Numeric;node.outputs[0].contract=owned(output);expect(resolve(node));
}
void converter_nan_identity(ConfiguredProcessingNode& node) {
    auto& policy=std::get<ConverterProcessingParameters>(node.parameters).policy;
    policy.nan_replacement=std::bit_cast<double>(std::uint64_t{0x7ff8000000001234});expect(resolve(node));
    auto duplicate=node.parameters;expect(same_processing_parameters(node.parameters,duplicate));
    std::get<ConverterProcessingParameters>(duplicate).policy.nan_replacement=std::bit_cast<double>(std::uint64_t{0x7ff8000000005678});
    expect(!same_processing_parameters(node.parameters,duplicate));
}
void converter_policy_rules() {
    const auto contract=vector_contract();auto node=converter_fixture(contract);converter_contract_rules(node);
    node.outputs[0].contract=owned(contract);converter_nan_identity(node);
    std::get<ConverterProcessingParameters>(node.parameters).policy.rounding=static_cast<Rounding>(77);
    expect(resolve(node).error==ProcessingError::InvalidParameters);
}
void resolver_tests() {
    extractor_and_display_tests();valid_routing_node();invalid_route_preserves_resolution();incompatible_routing_output();
    routing_selection_limits();sink_port_rules();source_role_rules();endpoint_parameter_rules();converter_policy_rules();
}
NumericContract reduction_scalar(const NumericContract& input) {
    NumericContract scalar;scalar.dtype=DType::UInt64;scalar.channel_order=std::span(channels).first(1);
    scalar.quantity=input.quantity;scalar.unit=input.unit;scalar.clock=input.clock;return scalar;
}
NumericContract accumulated_history(const NumericContract& input) {
    auto history=input;history.semantic=Semantic::History;history.history_element=input.semantic;
    history.shape.rank=Rank::Matrix;history.shape.axes={Axis::Time,Axis::Band};
    history.shape.extents={ExtentBounds{0,8},input.shape.extents[0]};history.coordinates={AxisCoordinates{},input.coordinates[0]};return history;
}
ConfiguredProcessingNode reduction_node(const NumericContract& input,const NumericContract& scalar) {
    ConfiguredProcessingNode node;node.id=5;node.kind="ScalarReduce";node.parameters=ReduceProcessingParameters{};
    node.inputs.push_back(port(0,PortDirection::Input,input));node.outputs.push_back(port(0,PortDirection::Output,scalar));return node;
}
void reduction_channel_selection(ConfiguredProcessingNode& node) {
    auto& parameters=std::get<ReduceProcessingParameters>(node.parameters);
    parameters.channels=ReductionChannels::BothChannels;expect(resolve(node).error==ProcessingError::InvalidParameters);
    parameters.channels=ReductionChannels::SelectedChannels;parameters.selected_channels={7};expect(resolve(node));
    parameters.selected_channels={999};expect(resolve(node).error==ProcessingError::InvalidParameters);
}
void reduction_duplicate_selection(ConfiguredProcessingNode& node) {
    auto& parameters=std::get<ReduceProcessingParameters>(node.parameters);
    parameters.selected_channels={7,7};expect(resolve(node).error==ProcessingError::InvalidParameters);
    parameters.selected_channels={7};
}
void reduction_result_type(ConfiguredProcessingNode& node,const NumericContract& scalar,NumericReduction operation) {
    std::get<ReduceProcessingParameters>(node.parameters).operation=operation;
    expect(resolve(node).error==ProcessingError::InvalidContract);
    auto result=scalar;result.dtype=DType::Float64;node.outputs[0].contract=owned(result);expect(resolve(node));
    node.outputs[0].contract=owned(scalar);
}
void reduction_history_shape(ConfiguredProcessingNode& node,const NumericContract& history,const NumericContract& scalar) {
    node.inputs[0].contract=owned(history);expect(resolve(node));
    // WholeValue Matrix reduction and explicit LatestHistoryRow are both supported.
    std::get<ReduceProcessingParameters>(node.parameters).rows=ReductionRows::LatestHistoryRow;expect(resolve(node));
    for(auto operation:{NumericReduction::SignedMean,NumericReduction::RMS})reduction_result_type(node,scalar,operation);
    std::get<ReduceProcessingParameters>(node.parameters).operation=NumericReduction::MaxAbs;
}
ConfiguredProcessingNode history_node(const NumericContract& input,const NumericContract& history,const NumericContract& scalar) {
    ConfiguredProcessingNode node;node.id=8;node.kind="Spectrogram";node.parameters=HistoryProcessingParameters{8};
    node.inputs.push_back(port(0,PortDirection::Input,input));node.outputs.push_back(port(history_output,PortDirection::Output,history));
    auto count=scalar;count.quantity=Quantity::FilledFrameCount;count.unit=NumericUnit::Frames;
    node.outputs.push_back(port(filled_count_output,PortDirection::Output,count));return node;
}
void reject_variable_history_geometry(ConfiguredProcessingNode& node,const NumericContract& input,const NumericContract& history) {
    auto variable=input;variable.shape.extents[0].minimum=1;node.inputs[0].contract=owned(variable);
    auto changed=history;changed.shape.extents[1]=variable.shape.extents[0];node.outputs[0].contract=owned(changed);
    expect(resolve(node).error==ProcessingError::InvalidContract);
    node.inputs[0].contract=owned(input);
}
void history_time_contracts(ConfiguredProcessingNode& node,const NumericContract& history) {
    auto regular=history;regular.coordinates[0]={CoordinateMode::Regular,NumericUnit::Seconds,{0,1},{1,60}};
    node.outputs[0].contract=owned(regular);expect(resolve(node).error==ProcessingError::InvalidContract);
    auto explicit_time=history;explicit_time.coordinates[0]={CoordinateMode::ExplicitFloat64LE,NumericUnit::Seconds};
    node.outputs[0].contract=owned(explicit_time);expect(resolve(node));node.outputs[0].contract=owned(history);
}
void history_count_contracts(ConfiguredProcessingNode& node) {
    const auto retained=node.outputs[1].contract;auto data=retained.view();auto& count=std::get<NumericContract>(data);
    count.dtype=DType::UInt32;node.outputs[1].contract=owned(count);expect(resolve(node).error==ProcessingError::InvalidContract);
    count.dtype=DType::UInt64;node.outputs[1].contract=owned(count);std::get<HistoryProcessingParameters>(node.parameters).capacity=9;
    expect(resolve(node).error==ProcessingError::IncompatiblePorts);
}
ConfiguredProcessingNode wled_encoder(const NumericContract& input,const StructuredContract& packet) {
    ConfiguredProcessingNode node;node.id=6;node.kind="WLEDEncode";node.parameters=TransformProcessingParameters{};
    node.inputs.push_back(port(0,PortDirection::Input,input));
    auto scalar=input;scalar.dtype=DType::Float32;scalar.representation.alignment=alignof(float);
    scalar.shape={};scalar.coordinates={};scalar.semantic=Semantic::Scalar;
    scalar.quantity=Quantity::Unspecified;scalar.unit=NumericUnit::Hertz;
    node.inputs.push_back(port(1,PortDirection::Input,scalar));
    scalar.quantity=Quantity::SpectrumPeakAmplitude;scalar.unit=NumericUnit::RawAmplitude;
    node.inputs.push_back(port(2,PortDirection::Input,scalar));
    node.outputs.push_back(port(0,PortDirection::Output,packet));return node;
}
void wled_rejected_contracts(ConfiguredProcessingNode& encoder,ConfiguredProcessingNode& receiver,const NumericContract& input) {
    auto packet=std::get<StructuredContract>(receiver.inputs[0].contract.view());packet.version=3;
    receiver.inputs[0].contract=owned(packet);expect(!processing_ports_compatible(encoder.outputs[0],receiver.inputs[0]));
    encoder.outputs[0].contract=owned(input);expect(resolve(encoder).error==ProcessingError::InvalidContract);
}
void wled_contract_tests(const NumericContract& input) {
    const StructuredContract packet{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};
    auto encoder=wled_encoder(input,packet);expect(resolve(encoder));auto receiver=sink(7,packet,"WLEDOutput");expect(resolve(receiver));
    expect(processing_ports_compatible(encoder.outputs[0],receiver.inputs[0]));wled_rejected_contracts(encoder,receiver,input);
}
double wled_declared_number(const ConfiguredProcessingNode& node,std::string_view key) {
    ResolvedProcessingNode resolved;expect(resolve_processing_node(node,limits,resolved));ProcessingParameterView value;
    expect(resolve_processing_parameter(resolved,key,value)==Status::ok);
    return std::visit([](auto value){return double(value);},std::get<NumericLiteral>(value));
}
void wled_declared_defaults(const ConfiguredProcessingNode& node) {
    expect(wled_declared_number(node,"peak_hz")==0);expect(wled_declared_number(node,"magnitude")==0);
}
void wled_explicit_peak_settings(ConfiguredProcessingNode& node) {
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    fields={{"peak_hz",NumericLiteral{131.25}},{"magnitude",NumericLiteral{.75}}};
    expect(resolve(node));expect(wled_declared_number(node,"peak_hz")==131.25);expect(wled_declared_number(node,"magnitude")==.75);
}
void reject_wled_peak_setting(ConfiguredProcessingNode& node,std::string_view key,double value) {
    std::get<TransformProcessingParameters>(node.parameters).values={{std::string(key),NumericLiteral{value}}};
    expect(resolve(node).error==ProcessingError::InvalidParameters);
}
void wled_peak_settings_contract() {
    const StructuredContract packet{"wled-audio-sync",1,"audio-sync-44-le",{44,44}};
    auto node=wled_encoder(vector_contract(),packet);wled_declared_defaults(node);wled_explicit_peak_settings(node);
    for(auto key:{"peak_hz","magnitude"}) {
        reject_wled_peak_setting(node,key,-1);reject_wled_peak_setting(node,key,std::numeric_limits<double>::infinity());
        reject_wled_peak_setting(node,key,double(std::numeric_limits<float>::max())*2);
    }
}
void reduction_contract_tests(const NumericContract& input,const NumericContract& scalar,const NumericContract& history) {
    auto node=reduction_node(input,scalar);expect(resolve(node));reduction_channel_selection(node);reduction_duplicate_selection(node);
    reduction_history_shape(node,history,scalar);
}
void history_contract_tests(const NumericContract& input,const NumericContract& scalar,const NumericContract& history) {
    auto node=history_node(input,history,scalar);expect(resolve(node));reject_variable_history_geometry(node,input,history);
    history_time_contracts(node,history);history_count_contracts(node);
}
void reduction_history_tests() {
    const auto input=vector_contract(),scalar=reduction_scalar(input),history=accumulated_history(input);
    reduction_contract_tests(input,scalar,history);history_contract_tests(input,scalar,history);wled_contract_tests(input);
    wled_peak_settings_contract();
}
struct QueryState {std::size_t calls=0;bool allow=true;std::size_t bytes=5;};
ProcessingSupport query(const void* context,const ResolvedProcessingNode& n,const ProcessingResourceProfile&) noexcept {
    auto& s=*const_cast<QueryState*>(static_cast<const QueryState*>(context));++s.calls;
    bool ok=s.allow;
    // Deliberately non-Cartesian combination: only UInt64 Mel, not arbitrary UInt64 or Mel.
    for(const auto& p:n.outputs){auto v=p.contract.view();if(const auto* c=std::get_if<NumericContract>(&v))ok=ok&&c->dtype==DType::UInt64&&c->semantic==Semantic::MelEnergy;}
    return {ok,{s.bytes,0,0,2}};
}
struct AdmissionFixture {
    NumericContract contract=vector_contract();
    NumericProcessingGraph graph;
    QueryState query_state;
    NumericDeviceCapabilities capabilities{&query_state,query,{{100,100,100,20},11,65536}};
    std::array<std::size_t,32> workspace{};
    ProcessingGraphAdmission admission{999,{7,8,9,10},123};
    explicit AdmissionFixture(bool routed=true) {
        graph.graph_id=routed?"actual.graph":"budget.graph";
        graph.nodes=routed?std::vector<ConfiguredProcessingNode>{source(1,contract),route(2,contract),sink(3,contract)}
            :std::vector<ConfiguredProcessingNode>{source(1,contract),sink(2,contract)};
        graph.edges=routed?std::vector<Edge>{{1,2,0,0},{2,3,1,0}}:std::vector<Edge>{{1,2,0,0}};
    }
    ProcessingDiagnostic admit() {return admit_processing_graph(graph,limits,capabilities,workspace,admission);}
    void expect_error(ProcessingError error){expect(admit().error==error);}
    void expect_retained(const ProcessingGraphAdmission& prior) {
        expect(admission.node_count==prior.node_count&&admission.peak_bytes==prior.peak_bytes);
    }
};
void initial_graph_admission(AdmissionFixture& fixture) {
    expect(fixture.admit());expect(fixture.admission.node_count==3&&fixture.admission.resources.plan_bytes==15&&fixture.admission.resources.stack_bytes==2);
    expect(fixture.query_state.calls==3);
}
void reject_duplicate_graph_input(AdmissionFixture& fixture,const ProcessingGraphAdmission& frozen) {
    fixture.graph.edges.push_back({1,3,0,0});fixture.expect_error(ProcessingError::MultipleInputs);
    fixture.expect_retained(frozen);fixture.graph.edges.pop_back();
}
void reject_missing_graph_endpoint(AdmissionFixture& fixture) {
    fixture.graph.edges[1].output_port=2;fixture.expect_error(ProcessingError::MissingEndpoint);
    fixture.graph.edges[1].output_port=1;
}
void reject_unsupported_graph_combination(AdmissionFixture& fixture) {
    auto changed=fixture.contract;changed.dtype=DType::Float64;fixture.graph.nodes[2].inputs[0].contract=owned(changed);
    fixture.expect_error(ProcessingError::UnsupportedCombination);fixture.graph.nodes[2]=sink(3,fixture.contract);
    fixture.query_state.allow=false;fixture.expect_error(ProcessingError::UnsupportedCombination);fixture.query_state.allow=true;
}
void reject_graph_resources(AdmissionFixture& fixture) {
    fixture.capabilities.profile.maximum.plan_bytes=14;fixture.expect_error(ProcessingError::ResourceLimit);
    fixture.capabilities.profile.maximum.plan_bytes=100;fixture.capabilities.profile.max_peak_bytes=1;
    fixture.expect_error(ProcessingError::ResourceLimit);fixture.capabilities.profile.max_peak_bytes=65536;
}
void reject_graph_workspace(AdmissionFixture& fixture) {
    expect(admit_processing_graph(fixture.graph,limits,fixture.capabilities,std::span(fixture.workspace).first(5),fixture.admission).error==ProcessingError::InsufficientWorkspace);
    auto tiny=limits;tiny.max_nodes=2;
    expect(admit_processing_graph(fixture.graph,tiny,fixture.capabilities,fixture.workspace,fixture.admission).error==ProcessingError::Limit);
}
void graph_version_boundaries(AdmissionFixture& fixture) {
    fixture.graph.versions.processing=1;fixture.expect_error(ProcessingError::UnsupportedVersion);
    fixture.graph.versions.processing=ProcessingVersions{}.processing;
    expect(processing_versions_supported(fixture.graph.versions)==Status::ok);
}
void reject_graph_cycles_and_identity(AdmissionFixture& fixture,const ProcessingGraphAdmission& frozen) {
    fixture.graph.nodes[2]=route(3,fixture.contract);fixture.graph.edges.push_back({3,2,0,0});fixture.graph.edges.erase(fixture.graph.edges.begin());
    fixture.expect_error(ProcessingError::Cycle);fixture.graph.nodes[2].id=2;fixture.expect_error(ProcessingError::InvalidNode);
    fixture.expect_retained(frozen);
}
void graph_tests() {
    AdmissionFixture fixture;initial_graph_admission(fixture);const auto frozen=fixture.admission;
    reject_duplicate_graph_input(fixture,frozen);reject_missing_graph_endpoint(fixture);reject_unsupported_graph_combination(fixture);
    reject_graph_resources(fixture);reject_graph_workspace(fixture);graph_version_boundaries(fixture);
    reject_graph_cycles_and_identity(fixture,frozen);
}
void reserved_capacity_admission(AdmissionFixture& fixture) {
    expect(fixture.admit());const auto before=fixture.admission;
    // Reserved DTO storage contributes to the retained memory budget.
    fixture.graph.nodes[0].outputs.reserve(3000);fixture.expect_error(ProcessingError::Limit);
    expect(fixture.admission.peak_bytes==before.peak_bytes);
}
ConfiguredProcessingNode coefficient_generator_fixture(const NumericContract& input) {
    auto ordinary=input;ordinary.semantic=Semantic::Window;ordinary.quantity=Quantity::Amplitude;ordinary.shape.axes[0]=Axis::Sample;
    ConfiguredProcessingNode node;node.id=10;node.kind="WindowFunction";node.parameters=TransformProcessingParameters{};
    node.inputs.push_back(port(0,PortDirection::Input,ordinary));node.outputs.push_back(port(0,PortDirection::Output,input));return node;
}
NumericContract coefficient_length_contract(const NumericContract& input) {
    auto scalar=input;scalar.semantic=Semantic::Scalar;scalar.dtype=DType::UInt32;scalar.shape={};scalar.coordinates={};
    scalar.quantity=Quantity::Unspecified;scalar.unit=NumericUnit::Values;scalar.representation={};
    scalar.representation.alignment=alignof(std::uint32_t);return scalar;
}
void configure_coefficient_generator(ConfiguredProcessingNode& node,const NumericContract& input) {
    expect(resolve(node).error==ProcessingError::IncompatiblePorts);
    const auto length=coefficient_length_contract(input);node.inputs[0].contract=owned(length);
    OwnedProcessingContract generated;expect(array_generator_output_contract(node,length,generated)==Status::ok);
    node.outputs[0].contract=generated;expect(resolve(node));
}
ProcessingParameterView window_parameter_value(const ConfiguredProcessingNode& node) {
    ResolvedProcessingNode resolved;expect(resolve_processing_node(node,limits,resolved));ProcessingParameterView value;
    expect(resolve_processing_parameter(resolved,"function",value)==Status::ok);return value;
}
void absent_parameter_preserves_value(const ConfiguredProcessingNode& node,ProcessingParameterView value) {
    ResolvedProcessingNode resolved;expect(resolve_processing_node(node,limits,resolved));
    expect(resolve_processing_parameter(resolved,"channel",value)==Status::invalid_argument);
    expect(std::get<std::string_view>(value)=="blackman");
}
void coefficient_parameter_defaults(const NumericContract& input) {
    auto node=coefficient_generator_fixture(input);configure_coefficient_generator(node,input);
    auto value=window_parameter_value(node);expect(std::get<std::string_view>(value)=="hann");
    std::get<TransformProcessingParameters>(node.parameters).values.push_back({"function",std::string("blackman")});
    value=window_parameter_value(node);expect(std::get<std::string_view>(value)=="blackman");
    absent_parameter_preserves_value(node,value);
}
void exact_integer_parameter_identity() {
    auto exact=ConverterProcessingParameters{};exact.policy.scaling.gain.numerator=std::numeric_limits<std::uint64_t>::max();
    auto other=exact;expect(same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
    other.policy.scaling.gain.numerator=std::uint64_t{9007199254740993};
    expect(!same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
}
void signed_zero_parameter_identity() {
    auto exact=ConverterProcessingParameters{};exact.policy.nan_replacement=-0.0;
    auto other=exact;other.policy.nan_replacement=0.0;
    expect(!same_processing_parameters(ProcessingParameters{exact},ProcessingParameters{other}));
}
void admission_peak_boundary(AdmissionFixture& fixture) {
    fixture.capabilities.profile.maximum.plan_bytes=std::numeric_limits<std::size_t>::max();
    fixture.capabilities.profile.max_peak_bytes=std::numeric_limits<std::size_t>::max();
    fixture.graph.nodes[0]=source(1,fixture.contract);fixture.query_state.bytes=std::numeric_limits<std::size_t>::max();
    fixture.expect_error(ProcessingError::ResourceLimit);fixture.query_state.bytes=5;expect(fixture.admit());
    const auto peak=fixture.admission.peak_bytes;fixture.capabilities.profile.max_peak_bytes=peak-1;
    fixture.expect_error(ProcessingError::ResourceLimit);fixture.capabilities.profile.max_peak_bytes=peak;
}
void admission_noalloc(AdmissionFixture& fixture) {
    const auto before=allocation_probe::allocations;expect(fixture.admit());expect(allocation_probe::allocations==before);
}
void missing_capability_query(AdmissionFixture& fixture) {
    fixture.capabilities.query=nullptr;fixture.expect_error(ProcessingError::UnsupportedCombination);fixture.capabilities.query=query;
    auto unsupported=fixture.contract;unsupported.dtype=DType::Float64;
    fixture.graph.nodes={source(1,unsupported),sink(2,unsupported)};fixture.expect_error(ProcessingError::UnsupportedCombination);
}
void expect_port_incompatible(const ProcessingPort& output,ProcessingPort& input,const NumericContract& contract) {
    input.contract=owned(contract);expect(!processing_ports_compatible(output,input));
}
void dtype_connection_dimensions(DType type) {
    const auto contract=vector_contract(type);auto output=port(0,PortDirection::Output,contract),input=port(0,PortDirection::Input,contract);
    expect(processing_ports_compatible(output,input));
    auto changed=contract;changed.dtype=type==DType::UInt64?DType::Float64:DType::UInt64;expect_port_incompatible(output,input,changed);
    changed=contract;changed.clock.sample_rate=1;expect_port_incompatible(output,input,changed);
    changed=contract;changed.representation.layout=ChannelLayout::Interleaved;expect_port_incompatible(output,input,changed);
}
void admission_boundary_tests() {
    AdmissionFixture fixture(false);reserved_capacity_admission(fixture);coefficient_parameter_defaults(fixture.contract);
    exact_integer_parameter_identity();signed_zero_parameter_identity();admission_peak_boundary(fixture);admission_noalloc(fixture);
    missing_capability_query(fixture);for(const auto& descriptor:dtype_descriptors())dtype_connection_dimensions(descriptor.type);
}
ProcessingSupport any_combination_query(const void* context,const ResolvedProcessingNode&,const ProcessingResourceProfile&) noexcept {
    auto& state=*const_cast<QueryState*>(static_cast<const QueryState*>(context));++state.calls;
    return {state.allow,{1,2,3,4}};
}
NumericContract pcm_vector_contract() {
    auto pcm=vector_contract(DType::Float32);pcm.semantic=Semantic::PCM;pcm.quantity=Quantity::Amplitude;
    pcm.shape.axes[0]=Axis::Sample;pcm.coordinates={};return pcm;
}
void canonical_versions_gate_admission(AdmissionFixture& fixture) {
    for(auto edition:{0u,1u,2u,3u,99u}) {
        fixture.graph.versions={};fixture.graph.versions.processing=edition;fixture.expect_error(ProcessingError::UnsupportedVersion);
    }
    for(auto edition:{0u,1u,2u,3u,4u,99u}) {
        fixture.graph.versions={};fixture.graph.versions.graph=edition;fixture.expect_error(ProcessingError::UnsupportedVersion);
    }
    fixture.graph.versions={};
}
ConfiguredProcessingNode scalar_window_fixture(const NumericContract& pcm) {
    auto scalar=pcm;scalar.shape={};scalar.channel_order=std::span(channels).first(1);scalar.clock.sample_rate=48000;
    auto window=pcm;window.semantic=Semantic::Window;window.shape.extents[0]={512,512};window.channel_order=scalar.channel_order;window.clock=scalar.clock;
    ConfiguredProcessingNode node;node.id=2;node.kind="Window";
    node.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{512}}},{"input_rate",NumericLiteral{std::uint64_t{48000}}}}};
    node.inputs={port(0,PortDirection::Input,scalar)};node.outputs={port(0,PortDirection::Output,window)};return node;
}
void scalar_window_device_admission(AdmissionFixture& fixture,const ConfiguredProcessingNode& node) {
    expect(resolve(node));fixture.graph.nodes={node};expect(fixture.admit());
    fixture.query_state.allow=false;const auto before=fixture.admission;fixture.expect_error(ProcessingError::UnsupportedCombination);
    fixture.expect_retained(before);fixture.query_state.allow=true;
}
void scalar_window_semantics(ConfiguredProcessingNode node) {
    node.kind="StereoFFT";node.parameters=TransformProcessingParameters{};expect(resolve(node).error==ProcessingError::IncompatiblePorts);
    node.kind="Window";node.parameters=TransformProcessingParameters{{{"size",NumericLiteral{std::uint64_t{512}}},{"input_rate",NumericLiteral{std::uint64_t{48000}}}}};
    const auto retained=node.inputs[0].contract;auto data=retained.view();std::get<NumericContract>(data).semantic=Semantic::Scalar;
    node.inputs[0].contract=owned(data);expect(resolve(node));
}
ConfiguredProcessingNode test_signal_fixture(const NumericContract& input) {
    auto pcm=input;pcm.clock.sample_rate=48000;auto node=source(3,pcm);node.kind="TestSignal";return node;
}
void invalid_signal_ranges(ConfiguredProcessingNode& signal) {
    auto& fields=std::get<EndpointProcessingParameters>(signal.parameters).values;
    fields={{"minimum_hz",NumericLiteral{1000.0}},{"maximum_hz",NumericLiteral{100.0}}};expect(resolve(signal).error==ProcessingError::InvalidParameters);
    fields={{"maximum_hz",NumericLiteral{30000.0}}};expect(resolve(signal).error==ProcessingError::InvalidParameters);
}
void invalid_signal_skips_device_query(AdmissionFixture& fixture,const ConfiguredProcessingNode& signal) {
    fixture.graph.nodes={signal};const auto calls=fixture.query_state.calls;
    fixture.expect_error(ProcessingError::InvalidParameters);expect(fixture.query_state.calls==calls);
}
void signal_defaults_preserve_parameters(ConfiguredProcessingNode& signal) {
    std::get<EndpointProcessingParameters>(signal.parameters).values.clear();const auto absent=signal.parameters;
    expect(resolve(signal));expect(same_processing_parameters(absent,signal.parameters));
}
void signal_nyquist_boundaries(ConfiguredProcessingNode& signal) {
    auto& fields=std::get<EndpointProcessingParameters>(signal.parameters).values;
    fields={{"maximum_hz",NumericLiteral{24000.0}},{"sweep_depth",NumericLiteral{0.0}}};expect(resolve(signal).error==ProcessingError::InvalidParameters);
    fields[0].value=NumericLiteral{23999.0};expect(resolve(signal));
    fields={{"minimum_hz",NumericLiteral{1000.0}},{"maximum_hz",NumericLiteral{1000.0}}};expect(resolve(signal).error==ProcessingError::InvalidParameters);
}
void signal_explicit_parameters_immutable(const ConfiguredProcessingNode& signal) {
    const auto parameters=signal.parameters;expect(resolve(signal));expect(same_processing_parameters(parameters,signal.parameters));
}
void signal_declared_rate_rules(ConfiguredProcessingNode& signal) {
    auto& fields=std::get<EndpointProcessingParameters>(signal.parameters).values;
    fields={{"sample_rate",NumericLiteral{std::uint64_t{8000}}}};expect(resolve(signal).error==ProcessingError::InvalidParameters);
    fields={{"maximum_hz",NumericLiteral{18000.0}},{"sample_rate",NumericLiteral{std::uint64_t{48000}}},{"sweep_depth",NumericLiteral{0.18}}};
    expect(resolve(signal));signal_explicit_parameters_immutable(signal);
}
void routing_stream_count_case(std::string_view kind,std::uint16_t count,const NumericContract& pcm) {
    auto node=route(4,pcm,std::string(kind));node.outputs.clear();node.parameters=RouteProcessingParameters{count,std::uint16_t(count-1),true};
    for(PortId id=0;id<count;++id)node.outputs.push_back(port(id,PortDirection::Output,pcm));
    if(count==2 || count==8)expect(resolve(node));else expect(resolve(node).error==ProcessingError::InvalidParameters);
}
void routing_stream_count_limits(const NumericContract& pcm) {
    for(auto kind:{"Splitter","Switch"})for(std::uint16_t count:{1,2,8,9})routing_stream_count_case(kind,count,pcm);
}
void independent_signal_repairs(AdmissionFixture& fixture,const NumericContract& pcm) {
    auto signal=test_signal_fixture(pcm);invalid_signal_ranges(signal);invalid_signal_skips_device_query(fixture,signal);
    signal_defaults_preserve_parameters(signal);signal_nyquist_boundaries(signal);signal_declared_rate_rules(signal);
}
void independent_findings_repairs() {
    AdmissionFixture fixture(false);fixture.capabilities.query=any_combination_query;
    fixture.capabilities.profile={{100,100,100,100},10,65536};
    const auto pcm=pcm_vector_contract();fixture.graph.graph_id="local.processing";fixture.graph.nodes={source(1,pcm)};fixture.graph.edges.clear();
    expect(fixture.admit());canonical_versions_gate_admission(fixture);
    auto window=scalar_window_fixture(pcm);scalar_window_device_admission(fixture,window);scalar_window_semantics(window);
    independent_signal_repairs(fixture,pcm);routing_stream_count_limits(pcm);
}
}
static NumericContract aligned_pcm_contract() {
    auto pcm=vector_contract(DType::Float32);pcm.shape.axes={Axis::Sample,Axis::None};pcm.coordinates={};
    pcm.clock={"local",{1,48000},48000};return pcm;
}
static ConfiguredProcessingNode capture_node(NodeId id,const NumericContract& pcm) {
    auto node=source(id,pcm);node.kind="AudioSource";node.parameters=EndpointProcessingParameters{};
    node.outputs.push_back(port(1,PortDirection::Output,pcm));return node;
}
static ConfiguredProcessingNode aligned_mixer(const NumericContract& pcm,NodeId id=4) {
    ConfiguredProcessingNode mixer;mixer.id=id;mixer.kind="Mixer";
    mixer.parameters=MixerProcessingParameters{MixerMode::WeightedMean,{{0,1},{1,1}},100,MixerConsumption::SourceAligned};
    mixer.inputs={port(0,PortDirection::Input,pcm),port(1,PortDirection::Input,pcm)};
    mixer.outputs={port(0,PortDirection::Output,pcm)};return mixer;
}
static ConfiguredProcessingNode grouped_route(const NumericContract& pcm) {
    auto node=route(3,pcm,"Switch");std::get<RouteProcessingParameters>(node.parameters).channels_per_stream=2;
    node.inputs.push_back(port(1,PortDirection::Input,pcm));node.outputs.push_back(port(2,PortDirection::Output,pcm));
    node.outputs.push_back(port(3,PortDirection::Output,pcm));return node;
}
static ProcessingSupport all_supported_query(const void*,const ResolvedProcessingNode&,const ProcessingResourceProfile&) noexcept {
    return {true,{}};
}
struct AlignedFixture {
    NumericContract pcm=aligned_pcm_contract();
    ConfiguredProcessingNode mixer=aligned_mixer(pcm),grouped=grouped_route(pcm);
    NumericProcessingGraph graph;
    NumericDeviceCapabilities device{nullptr,all_supported_query,{{65536,65536,65536,65536},65536,1048576}};
    std::array<std::size_t,32> workspace{};
    ProcessingGraphAdmission result;
    AlignedFixture() {
        graph.graph_id="aligned.lanes";graph.nodes={capture_node(1,pcm),capture_node(2,pcm),grouped,mixer};
        graph.edges={{1,3,0,0},{2,3,0,1},{3,4,0,0},{3,4,1,1}};
    }
    ProcessingDiagnostic admit(){return admit_processing_graph(graph,limits,device,workspace,result);}
    void switch_ancestry() {
        expect(admit().error==ProcessingError::IncompatiblePorts);graph.edges[1]={1,3,1,1};expect(admit());
        // Nonzero groups map modulo their explicit channels_per_stream.
        graph.edges[2].output_port=2;graph.edges[3].output_port=3;expect(admit());
    }
    void use_selector() {
        auto selector=grouped;selector.kind="InputSelector";
        selector.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
        selector.inputs={port(0,PortDirection::Input,pcm),port(1,PortDirection::Input,pcm),port(2,PortDirection::Input,pcm),port(3,PortDirection::Input,pcm)};
        selector.outputs.resize(2);graph.nodes[2]=selector;
        graph.edges={{1,3,0,0},{1,3,1,1},{2,3,0,2},{1,3,1,3},{3,4,0,0},{3,4,1,1}};
    }
    void selector_ancestry() {
        expect(admit().error==ProcessingError::IncompatiblePorts);graph.edges[3]={2,3,1,3};expect(admit());
    }
    void disconnected_capture_lane() {
        auto silent=route(6,pcm,"Gate");silent.parameters=RouteProcessingParameters{1,0,true};silent.outputs.resize(1);
        graph.nodes.push_back(silent);graph.edges[3]={6,3,0,3};expect(admit());
        graph.nodes.pop_back();graph.edges[3]={2,3,1,3};
    }
    void use_generators() {
        ConfiguredProcessingNode clock;clock.id=5;clock.kind="SampleClock";clock.parameters=SampleClockProcessingParameters{};
        for(std::size_t index=0;index<2;++index) {
            graph.nodes[index].kind="SignalGenerator";graph.nodes[index].parameters=SignalGeneratorProcessingParameters{};graph.nodes[index].outputs.resize(1);
        }
        graph.nodes={graph.nodes[0],graph.nodes[1],mixer,clock};graph.edges={{1,4,0,0},{2,4,0,1}};
        graph.configuration_bindings={{5,1},{5,2}};
    }
    void independent_generator_clocks() {
        expect(admit());auto other=graph.nodes.back();other.id=6;graph.nodes.push_back(other);
        graph.configuration_bindings[1].from=6;expect(admit().error==ProcessingError::IncompatiblePorts);
    }
};
static void aligned_ancestry_regressions() {
    AlignedFixture fixture;fixture.switch_ancestry();fixture.use_selector();fixture.selector_ancestry();
    fixture.disconnected_capture_lane();fixture.use_generators();fixture.independent_generator_clocks();
}
static NumericProcessingGraph clock_binding_fixture() {
    const auto pcm=aligned_pcm_contract();NumericProcessingGraph graph;graph.graph_id="config";
    ConfiguredProcessingNode clock;clock.id=1;clock.kind="SampleClock";clock.parameters=SampleClockProcessingParameters{};
    ConfiguredProcessingNode generator;generator.id=2;generator.kind="SignalGenerator";generator.parameters=SignalGeneratorProcessingParameters{};
    generator.outputs={port(0,PortDirection::Output,pcm)};generator.clock_node=999;
    graph.nodes={clock,generator};graph.configuration_bindings={{1,2}};return graph;
}
static NumericProcessingGraph resolve_clock_binding(const NumericProcessingGraph& graph) {
    NumericProcessingGraph resolved;expect(resolve_configuration_bindings(graph,limits,resolved));
    expect(resolved.nodes[1].clock_node==1);expect(graph.nodes[1].clock_node==999);return resolved;
}
static void duplicate_clock_binding_refusal(NumericProcessingGraph& graph,NumericProcessingGraph& resolved) {
    graph.configuration_bindings.push_back({1,2});const auto retained=resolved;
    expect(!resolve_configuration_bindings(graph,limits,resolved));expect(resolved.nodes[1].clock_node==retained.nodes[1].clock_node);
}
static void clock_binding_direction_and_removal(NumericProcessingGraph& graph,NumericProcessingGraph& resolved) {
    graph.configuration_bindings={{2,1}};expect(!resolve_configuration_bindings(graph,limits,resolved));
    graph.configuration_bindings.clear();expect(resolve_configuration_bindings(graph,limits,resolved));expect(resolved.nodes[1].clock_node==-1);
}
static void grouped_route_arity_refusal() {
    auto grouped=grouped_route(aligned_pcm_contract());grouped.id=5;expect(resolve(grouped));
    grouped.outputs.pop_back();expect(!resolve(grouped));
}
static void configuration_binding_regressions() {
    auto graph=clock_binding_fixture();auto resolved=resolve_clock_binding(graph);
    duplicate_clock_binding_refusal(graph,resolved);clock_binding_direction_and_removal(graph,resolved);grouped_route_arity_refusal();
}
static NumericProcessingGraph invalid_conversion_fixture() {
    auto input=vector_contract();input.dtype=DType::Float64;auto output=input;output.dtype=DType::Int16;
    NumericProcessingGraph graph;graph.graph_id="original.edge.index";graph.nodes={source(1,input),sink(2,output),sink(3,output)};
    graph.edges={{1,3,0,0},{1,2,0,0}};graph.edges[1].conversion=255;return graph;
}
static void lowering_original_edge(const NumericProcessingGraph& graph,NumericProcessingGraph& untouched) {
    const auto diagnostic=lower_dtype_edges(graph,limits,untouched);
    expect(!diagnostic&&diagnostic.edge==1&&untouched.graph_id=="unchanged"&&untouched.nodes.empty());
}
static void execution_interval_boundaries() {
    expect(valid_execution_interval({1,60})&&valid_execution_interval({1,1000})&&valid_execution_interval({60,1})&&valid_execution_interval({7,333})&&valid_execution_interval({INT64_MAX,UINT64_MAX}));
    for(const Ratio ratio:{Ratio{0,1},Ratio{-1,1},Ratio{1,0},Ratio{2,120},Ratio{1,1001},Ratio{61,1},Ratio{INT64_MAX,1}})
        expect(!valid_execution_interval(ratio));
}
static void lowering_refuses_before_allocation(const NumericProcessingGraph& graph,NumericProcessingGraph& untouched) {
    auto oversized=graph;oversized.nodes.front().kind.assign(limits.max_owned_metadata_bytes,'x');
    const auto before=allocation_probe::allocations;
    expect(!lower_dtype_edges(oversized,limits,untouched)&&allocation_probe::allocations==before&&untouched.graph_id=="unchanged");
}
static void lowering_interval_regressions() {
    const auto graph=invalid_conversion_fixture();NumericProcessingGraph untouched;untouched.graph_id="unchanged";
    lowering_original_edge(graph,untouched);execution_interval_boundaries();lowering_refuses_before_allocation(graph,untouched);
}
static NumericContract physical_window_contract(std::uint32_t rate,std::uint64_t size) {
    auto window=vector_contract(DType::Float32);window.semantic=Semantic::Windowed;window.quantity=Quantity::Amplitude;
    window.shape.axes={Axis::Sample,Axis::None};window.shape.extents[0]={size,size};window.coordinates={};
    window.clock={"matrix.clock",{1,rate},rate};return window;
}
static NumericContract physical_spectrum_contract(const NumericContract& window,std::uint32_t rate,std::uint64_t size) {
    auto spectrum=window;spectrum.semantic=Semantic::SpectrumMagnitude;spectrum.shape.axes[0]=Axis::Bin;
    spectrum.shape.extents[0]={size/2,size/2};spectrum.coordinates[0]={CoordinateMode::Regular,NumericUnit::Hertz,{0,1},{std::int64_t(rate),size}};return spectrum;
}
static ConfiguredProcessingNode physical_fourier_node(const NumericContract& window,const NumericContract& spectrum) {
    ConfiguredProcessingNode fft;fft.id=2;fft.kind="StereoFFT";fft.parameters=TransformProcessingParameters{};
    fft.inputs={port(0,PortDirection::Input,window)};fft.outputs={port(0,PortDirection::Output,spectrum)};return fft;
}
struct PhysicalConfigurationFixture {
    std::uint32_t rate;
    std::uint64_t size;
    NumericContract window,spectrum;
    NumericProcessingGraph graph,resolved;
    PhysicalConfigurationFixture(std::uint32_t rate,std::uint64_t size):rate(rate),size(size),
        window(physical_window_contract(rate,size)),spectrum(physical_spectrum_contract(window,rate,size)) {
        graph.graph_id="physical.matrix";graph.nodes={source(1,window),physical_fourier_node(window,spectrum),sink(3,spectrum,"FFTView")};
        graph.edges={{1,2,0,0},{2,3,0,0}};
        graph.configuration_bindings={{2,3,ConfigurationRole::SampleClock,ConfigurationRole::SampleClock},{2,3,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid}};
    }
    void materialize() {
        expect(resolve_configuration_bindings(graph,limits,resolved));for(const auto& node:resolved.nodes)expect(resolve(node));
    }
    void check_coordinates() const {
        const auto actual=std::get<NumericContract>(resolved.nodes[2].inputs[0].contract.view());
        expect(actual.clock.sample_rate==rate&&actual.shape.extents[0].maximum==size/2&&actual.coordinates[0].step==Ratio(std::int64_t(rate),size));
        expect(has_configuration_binding(resolved,3,ConfigurationRole::SampleClock)&&has_configuration_binding(resolved,3,ConfigurationRole::FrequencyGrid));
    }
    void conflicting_grid() {
        const auto retained=resolved;auto conflict=graph;auto changed=spectrum;changed.coordinates[0].step={1,1};
        conflict.nodes[2].inputs[0].contract=owned(changed);
        expect(resolve_configuration_bindings(conflict,limits,resolved).error==ProcessingError::IncompatiblePorts);
        expect(resolved.graph_id==retained.graph_id&&resolved.configuration_bindings==retained.configuration_bindings&&same_contract(resolved.nodes[2].inputs[0].contract.view(),retained.nodes[2].inputs[0].contract.view()));
    }
    void missing_grid() {
        auto missing=graph;missing.configuration_bindings.pop_back();expect(resolve_configuration_bindings(missing,limits,resolved));
        expect(!has_configuration_binding(resolved,3,ConfigurationRole::FrequencyGrid));expect(same_contract(resolved.nodes[2].inputs[0].contract.view(),spectrum));
    }
    void reject_cycle(const NumericProcessingGraph& cyclic,const NumericProcessingGraph& before) {
        expect(!resolve_configuration_bindings(cyclic,limits,resolved));
        expect(resolved.configuration_bindings==before.configuration_bindings&&same_contract(resolved.nodes[2].inputs[0].contract.view(),before.nodes[2].inputs[0].contract.view()));
    }
    void dependency_cycles() {
        auto cyclic=graph;cyclic.configuration_bindings={{2,3,ConfigurationRole::SampleClock,ConfigurationRole::SampleClock},{3,2,ConfigurationRole::SampleClock,ConfigurationRole::SampleClock}};
        const auto before=resolved;reject_cycle(cyclic,before);
        cyclic.configuration_bindings={{2,2,ConfigurationRole::FrequencyGrid,ConfigurationRole::FrequencyGrid}};
        reject_cycle(cyclic,before);
    }
};
static void physical_configuration_case(std::uint32_t rate,std::uint64_t size) {
    PhysicalConfigurationFixture fixture(rate,size);fixture.materialize();fixture.check_coordinates();
    fixture.conflicting_grid();fixture.missing_grid();fixture.dependency_cycles();
}
static void physical_configuration_matrix() {
    for(const auto rate:{44100u,48000u,96000u})for(const std::uint64_t size:{512u,4096u})physical_configuration_case(rate,size);
}
static ConfiguredProcessingNode selector_node(const NumericContract& input,const NumericContract& output) {
    ConfiguredProcessingNode node;node.id=12;node.kind="InputSelector";
    node.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{42}}}}};
    node.inputs={port(7,PortDirection::Input,input),port(42,PortDirection::Input,input)};
    // Output IDs are canonical; sparse input identity is retained.
    node.outputs={port(0,PortDirection::Output,output)};return node;
}
static void selector_output_mismatch(const NumericContract& input,int mismatch) {
    auto output=input;
    if(mismatch==0)output.shape.extents[0]={8,8};
    if(mismatch==1)output.clock.domain="other.clock";
    if(mismatch==2)output.dtype=DType::Float64;
    if(mismatch==3)output.representation.encoding=StorageEncoding::FixedLittleEndian;
    expect(resolve(selector_node(input,output)).error==ProcessingError::IncompatiblePorts);
}
static void selector_unclocked_envelope(const NumericContract& samples,const NumericContract& envelope) {
    auto unclocked=samples;unclocked.clock.sample_rate=0;auto changed=envelope;changed.clock.sample_rate=0;
    expect(resolve(selector_node(unclocked,unclocked)));
    expect(resolve(selector_node(unclocked,changed)).error==ProcessingError::IncompatiblePorts);
}
static void selector_semantic_relabelling(const NumericContract& samples,const NumericContract& envelope) {
    auto node=selector_node(samples,envelope);expect(resolve(node));
    auto changed=envelope;changed.semantic=envelope.semantic==Semantic::PCM?Semantic::Numeric:Semantic::PCM;
    node.outputs[0].contract=owned(changed);expect(resolve(node));
}
static void expect_selector_incompatible(const NumericContract& samples,const NumericContract& output) {
    expect(resolve(selector_node(samples,output)).error==ProcessingError::IncompatiblePorts);
}
static void selector_sample_dimensions(const NumericContract& samples,const NumericContract& envelope) {
    auto changed=envelope;changed.shape.extents[0]={1,3};expect_selector_incompatible(samples,changed);
    changed=envelope;changed.clock={"samples",{1,44100},44100};expect_selector_incompatible(samples,changed);
    changed=envelope;changed.dtype=DType::Float64;expect_selector_incompatible(samples,changed);
    changed=envelope;changed.representation.encoding=StorageEncoding::FixedLittleEndian;expect_selector_incompatible(samples,changed);
}
static ConfiguredProcessingNode grouped_selector(const NumericContract& samples,const NumericContract& envelope) {
    auto node=selector_node(samples,envelope);
    node.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{1}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
    node.inputs={port(3,PortDirection::Input,samples),port(1,PortDirection::Input,samples),port(0,PortDirection::Input,samples),port(2,PortDirection::Input,samples)};
    node.outputs={port(1,PortDirection::Output,envelope),port(0,PortDirection::Output,envelope)};return node;
}
static void selector_grouped_identity(const NumericContract& samples,const NumericContract& envelope) {
    auto node=grouped_selector(samples,envelope);expect(resolve(node));
    auto invalid=envelope;invalid.dtype=DType::Float64;node.outputs[1].contract=owned(invalid);
    expect(resolve(node).error==ProcessingError::IncompatiblePorts);
}
static void selector_sample_envelopes() {
    auto samples=vector_contract(DType::Float32);samples.shape.axes={Axis::Sample,Axis::None};samples.coordinates={};
    samples.clock={"samples",{1,48000},48000};auto envelope=samples;envelope.shape.extents[0]={1,16};
    selector_unclocked_envelope(samples,envelope);
    for(auto label:{Semantic::PCM,Semantic::Numeric}) {
        samples.semantic=label;envelope.semantic=label;selector_semantic_relabelling(samples,envelope);selector_sample_dimensions(samples,envelope);
    }
    selector_grouped_identity(samples,envelope);
}
static void input_selector_output_regressions() {
    const auto bands=vector_contract(DType::Float32);expect(resolve(selector_node(bands,bands)));
    for(int mismatch=0;mismatch<4;++mismatch)selector_output_mismatch(bands,mismatch);
    selector_sample_envelopes();
}
NumericProcessingGraph late_edge_fixture() {
    auto value=vector_contract(DType::Float32);
    NumericProcessingGraph graph;graph.graph_id="late-edge";graph.nodes={source(1,value),sink(2,value)};
    graph.edges={{1,2},{999,2}};
    return graph;
}
void explicit_preparation_diagnostic_regressions() {
    expect(!ProcessingDiagnostic{}.edge.has_value());const auto graph=late_edge_fixture();
    NumericProcessingGraph evaluated,executable;
    const auto result=resolve_numeric_parameter_bindings(graph,limits,evaluated,executable);
    expect(result.error==ProcessingError::MissingEndpoint && result.edge==std::optional<std::size_t>{1});
    expect(evaluated.nodes.empty() && executable.nodes.empty());
}
NumericProcessingGraph preparation_provider_fixture() {
    auto scalar=vector_contract(DType::UInt32);scalar.shape={};scalar.coordinates={};scalar.unit=NumericUnit::Values;
    NumericProcessingGraph graph;graph.graph_id="prepare-provider";graph.nodes={source(1,scalar)};
    auto routed=route(2,scalar,"Reroute");std::get<RouteProcessingParameters>(routed.parameters).outputs=1;routed.outputs.resize(1);
    graph.nodes.push_back(routed);graph.edges={{1,2}};return graph;
}
void routed_provider_value(const NumericProcessingGraph& graph) {
    NumericLiteral result;
    PreparationValueProvider provider{nullptr,[](const void*,const NumericProcessingGraph&,NodeId id,PortId,NumericLiteral& out) noexcept {
        if(id!=1)return Status::unavailable;
        out=std::uint64_t{64};return Status::ok;
    }};
    expect(resolve_preparation_value(graph,2,0,result,&provider)==Status::ok);
    expect(std::get<std::uint64_t>(result)==64);
}
void explicit_preparation_provider_regressions() {
    const auto graph=preparation_provider_fixture();NumericLiteral result;
    expect(!preparation_value_capability(graph.nodes[0],0));expect(resolve_preparation_value(graph,1,0,result)==Status::unavailable);
    routed_provider_value(graph);
}
NumericContract diagnostic_spectrum() {
    auto input=vector_contract(DType::Float32);
    input.shape.axes[0]=Axis::Bin;input.shape.extents[0]={5,5};input.coordinates={};
    input.clock.sample_rate=8000;input.quantity=Quantity::SpectrumPower;input.unit=NumericUnit::SquaredAmplitude;
    return input;
}

ConfiguredProcessingNode profile_normalizer_fixture() {
    ConfiguredProcessingNode node;node.kind="FrequencyNormalizer";
    node.parameters=TransformProcessingParameters{{{"mode",std::string{"profile"}},{"frontend_declared",true},
        {"sample_rate",NumericLiteral{std::uint64_t{8000}}},{"fft_size",NumericLiteral{std::uint64_t{8}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"gains",NumericArray{{1,1,1,1,1}}}}};return node;
}

void profile_reason_case(std::string_view key,ProcessingParameterValue value,ProcessingReason reason) {
    auto node=profile_normalizer_fixture();node.id=70;
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    const auto found=std::find_if(fields.begin(),fields.end(),[key](const auto& field){return field.key==key;});
    if(found==fields.end())fields.push_back({std::string(key),std::move(value)});
    else found->value=std::move(value);
    const auto diagnostic=frequency_normalizer_diagnostic(node,diagnostic_spectrum());
    expect(!diagnostic);expect(diagnostic.reason==reason);expect(diagnostic.parameter==key);expect(diagnostic.node==70);
}
void profile_reason_regressions() {
    profile_reason_case("fft_size",NumericLiteral{std::uint64_t(7)},ProcessingReason::FftSize);
    profile_reason_case("sample_rate",NumericLiteral{std::uint64_t(9000)},ProcessingReason::SampleRate);
    profile_reason_case("first_bin",NumericLiteral{std::uint64_t(1)},ProcessingReason::SpectrumLayout);
    profile_reason_case("window_function",std::string("unknown"),ProcessingReason::WindowFunction);
    profile_reason_case("coherent_gain",NumericLiteral{-1.},ProcessingReason::CoherentGain);
    profile_reason_case("power_gain",NumericLiteral{-1.},ProcessingReason::PowerGain);
    profile_reason_case("frontend_declared",false,ProcessingReason::Frontend);
    profile_reason_case("resampler_mode",std::string("unknown"),ProcessingReason::Resampler);
    profile_reason_case("representation",std::string("psd"),ProcessingReason::SpectrumRepresentation);
    profile_reason_case("gains",NumericArray{{1}},ProcessingReason::GainCount);
    profile_reason_case("gains",NumericArray{{1,1,0,1,1}},ProcessingReason::GainValue);
}
void check_profile_gain(ConfiguredProcessingNode& node,const NumericContract& input,float gain,bool valid) {
    auto& gains=std::get<NumericArray>(std::get<TransformProcessingParameters>(node.parameters).values.back().value).values;
    gains[0]=gain;expect((validate_frequency_normalizer(node,input)==Status::ok)==valid);
}
void profile_gain_admission_regressions() {
    auto input=vector_contract(DType::Float32);input.shape.axes[0]=Axis::Bin;input.shape.extents[0]={5,5};input.coordinates={};
    input.clock={"profile",{1,8000},8000};input.quantity=Quantity::SpectrumPower;input.unit=NumericUnit::SquaredAmplitude;
    auto node=profile_normalizer_fixture();expect(validate_frequency_normalizer(node,input)==Status::ok);
    check_profile_gain(node,input,0,false);check_profile_gain(node,input,maximum_profile_gain+1,false);
    check_profile_gain(node,input,maximum_profile_gain,true);
}
ConfiguredProcessingNode heterogeneous_selector(const NumericContract& samples,const NumericContract& scalar) {
    ConfiguredProcessingNode node;node.id=1;node.kind="InputSelector";
    node.parameters=TransformProcessingParameters{{{"selected_input",NumericLiteral{std::uint64_t{0}}},{"channels_per_stream",NumericLiteral{std::uint64_t{2}}}}};
    node.inputs={port(0,PortDirection::Input,samples),port(1,PortDirection::Input,scalar),port(2,PortDirection::Input,samples),port(3,PortDirection::Input,scalar)};
    node.outputs={port(1,PortDirection::Output,scalar),port(0,PortDirection::Output,samples)};return node;
}
void check_heterogeneous_selector_lanes(const ConfiguredProcessingNode& node,const NumericContract& scalar,const NumericContract& samples) {
    expect(same_physical_contract(node.outputs[0].contract.view(),scalar));expect(same_physical_contract(node.outputs[1].contract.view(),samples));
}
void selector_reordered_scalar_regressions() {
    auto samples=aligned_pcm_contract();samples.clock.domain="samples";
    auto scalar=samples;scalar.dtype=DType::UInt32;scalar.shape={};scalar.unit=NumericUnit::Hertz;
    auto node=heterogeneous_selector(samples,scalar);expect(derive_processing_node_contracts(node,limits));
    check_heterogeneous_selector_lanes(node,scalar,samples);expect(resolve(node));
}
void expect_preparation_literal(const NumericProcessingGraph& graph,NodeId id,PortId output,const NumericLiteral& expected) {
    NumericLiteral value;expect(resolve_preparation_value(graph,id,output,value)==Status::ok);
    expect(value.index()==expected.index()&&value==expected);
}
NumericProcessingGraph declared_generator_fixture() {
    ConfiguredProcessingNode generator;
    expect(make_processing_node(NodeKind::SignalGenerator,1,limits,generator,NodeFactorySettings{44100,512,256,400}));
    const auto clock=signal_generator_clock(generator);expect(clock&&clock->sample_rate==44100);
    NumericProcessingGraph graph;graph.graph_id="unbound-generator";graph.nodes={generator};return graph;
}
void unbound_generator_declared_clock_regression() {
    const auto graph=declared_generator_fixture();expect_preparation_literal(graph,1,sample_rate_output,NumericLiteral{std::uint64_t{44100}});
    NumericProcessingGraph executable;std::vector<RoutingAlias> aliases;
    expect(lower_numeric_metadata_outputs(graph,limits,executable,aliases));
    expect(executable.nodes.size()==2&&executable.nodes.back().kind=="Number");
}
ConfiguredProcessingNode bypass_rate_fixture(bool enabled,std::string mode="existing_fir") {
    ConfiguredProcessingNode node;
    expect(make_processing_node(NodeKind::PcmResampler,2,limits,node,NodeFactorySettings{44100,512,256,400}));
    node.parameters=TransformProcessingParameters{{{"input_rate",NumericLiteral{std::uint64_t{44100}}},
        {"output_rate",NumericLiteral{std::uint64_t{32000}}},{"mode",std::move(mode)}}};
    node.enabled=enabled;expect(derive_processing_node_contracts(node,limits));return node;
}
void bypass_declared_rate_case(bool disabled) {
    const auto node=bypass_rate_fixture(!disabled,disabled?"existing_fir":"bypass");
    NumericProcessingGraph graph;graph.graph_id="explicit-bypass-fs";graph.nodes={node};
    expect_preparation_literal(graph,2,sample_rate_output,NumericLiteral{std::uint64_t{44100}});
    expect(same_physical_contract(node.inputs.front().contract.view(),node.outputs.front().contract.view()));expect(resolve(node));
}
void bypass_declared_sample_rate_regression() {
    for(bool disabled:{false,true})bypass_declared_rate_case(disabled);
}
void resampler_declared_input_errors(ConfiguredProcessingNode& node) {
    auto& fields=std::get<TransformProcessingParameters>(node.parameters).values;
    fields.front().value=NumericLiteral{std::uint64_t{0}};expect(resolve(node).error==ProcessingError::PreparationValueUnavailable);
    fields.front().value=NumericLiteral{std::uint64_t{32000}};expect(resolve(node).error==ProcessingError::IncompatiblePorts);
}
void resampler_input_rate_binding_regression() {
    auto node=bypass_rate_fixture(false);resampler_declared_input_errors(node);
    ConfiguredProcessingNode clock;expect(make_processing_node(NodeKind::Fs,1,limits,clock,NodeFactorySettings{44100,512,256,400}));
    NumericProcessingGraph graph;graph.graph_id="resampler-explicit-input-fs";graph.nodes={clock,node};graph.edges={{1,2,0,input_sample_rate_input}};
    NumericProcessingGraph evaluated,executable;expect(resolve_numeric_parameter_bindings(graph,limits,evaluated,executable));
    expect_preparation_literal(executable,2,sample_rate_output,NumericLiteral{std::uint64_t{44100}});
}
void reject_bypass_output(ConfiguredProcessingNode& node,const NumericContract& contract) {
    node.outputs[0].contract=owned(contract);expect(resolve(node).error==ProcessingError::IncompatiblePorts);
}
void direct_bypass_admission_case(bool disabled) {
    auto node=bypass_rate_fixture(!disabled,disabled?"existing_fir":"bypass");expect(resolve(node));
    const auto saved=node.outputs[0].contract;auto wrong=std::get<NumericContract>(saved.view());
    wrong.clock.sample_rate=32000;wrong.clock.tick_seconds={1,32000};reject_bypass_output(node,wrong);
    node.outputs[0].contract=saved;wrong=std::get<NumericContract>(saved.view());wrong.shape.extents[0]={1,128};reject_bypass_output(node,wrong);
}
void direct_bypass_admission_regression() {
    for(bool disabled:{false,true})direct_bypass_admission_case(disabled);
}
struct SuppliedPreparation {
    Status status=Status::ok;NumericLiteral value=std::uint64_t{44100};
    static Status resolve(const void* context,const NumericProcessingGraph&,NodeId id,PortId,NumericLiteral& out) noexcept {
        if(id!=1)return Status::unavailable;
        const auto& supplied=*static_cast<const SuppliedPreparation*>(context);out=supplied.value;return supplied.status;
    }
};
NumericProcessingGraph preparation_failure_fixture(DType type) {
    auto node=bypass_rate_fixture(false);
    auto scalar=std::get<NumericContract>(node.inputs.back().contract.view());scalar.dtype=type;
    auto input=std::get<NumericContract>(node.inputs.front().contract.view());
    NumericProcessingGraph graph;graph.graph_id="typed-provider-failure";graph.nodes={source(4,input),source(1,scalar),node};
    graph.edges={{4,2},{1,2,0,input_sample_rate_input}};
    return graph;
}
void preparation_failure_atomicity(const ProcessingDiagnostic& result,const NumericProcessingGraph& evaluated,const NumericProcessingGraph& executable) {
    expect(!result&&result.node==2&&result.port==input_sample_rate_input&&result.edge==std::optional<std::size_t>{1});
    expect(evaluated.nodes.empty()&&executable.nodes.empty());
}
ProcessingDiagnostic preparation_failure_case(const SuppliedPreparation& supplied,DType type=DType::UInt32) {
    const auto graph=preparation_failure_fixture(type);
    NumericProcessingGraph evaluated,executable;
    PreparationValueProvider provider{&supplied,SuppliedPreparation::resolve};
    const auto result=resolve_numeric_parameter_bindings(graph,limits,evaluated,executable,-1,&provider);
    preparation_failure_atomicity(result,evaluated,executable);return result;
}
struct PreparationFailureExpectation {
    SuppliedPreparation supplied;
    ProcessingError error;
    DType type=DType::UInt32;
};
void expect_preparation_failure(const PreparationFailureExpectation& expected) {
    expect(preparation_failure_case(expected.supplied,expected.type).error==expected.error);
}
void preparation_failure_reason_regression() {
    const std::array cases{
        PreparationFailureExpectation{{Status::unavailable,std::uint64_t{44100}},ProcessingError::PreparationValueUnavailable},
        PreparationFailureExpectation{{Status::invalid_argument,std::uint64_t{44100}},ProcessingError::PreparationValueInvalid},
        PreparationFailureExpectation{{Status::exhausted,std::uint64_t{44100}},ProcessingError::ResourceLimit},
        PreparationFailureExpectation{{Status::overflow,std::uint64_t{44100}},ProcessingError::PreparationValueOutOfRange},
        PreparationFailureExpectation{{Status::incompatible_contract,std::uint64_t{44100}},ProcessingError::PreparationTypeMismatch},
        PreparationFailureExpectation{{Status::ok,std::uint64_t{0}},ProcessingError::PreparationValueOutOfRange},
        PreparationFailureExpectation{{Status::ok,1.0},ProcessingError::PreparationTypeMismatch,DType::Float64},
        PreparationFailureExpectation{{Status::ok,1.5},ProcessingError::PreparationValueOutOfRange},
        PreparationFailureExpectation{{Status::ok,std::uint64_t{UINT64_MAX}},ProcessingError::PreparationValueOutOfRange},
        PreparationFailureExpectation{{Status::ok,std::numeric_limits<double>::infinity()},ProcessingError::PreparationValueInvalid},
        PreparationFailureExpectation{{Status::ok,-std::numeric_limits<double>::infinity()},ProcessingError::PreparationValueInvalid},
        PreparationFailureExpectation{{Status::ok,std::numeric_limits<double>::quiet_NaN()},ProcessingError::PreparationValueInvalid}};
    for(const auto& expected:cases)expect_preparation_failure(expected);
}
const ParameterDescriptor* boundaries_descriptor() {
    const auto* descriptor=parameter_descriptor(NodeKind::BroadbandFeatures,"boundaries_hz");
    expect(descriptor && descriptor->default_array.size()==17);
    return descriptor;
}
void descriptor_boundary_array_default() {
    const auto* descriptor=boundaries_descriptor();
    ConfiguredProcessingNode node;node.kind="BroadbandFeatures";node.parameters=TransformProcessingParameters{};
    ResolvedProcessingNode resolved{node.id,node.kind,NodeRole::Processor,ProcessingOperation::MusicTransform,node.inputs,node.outputs,&node.parameters,true};
    ProcessingParameterView field;expect(resolve_processing_parameter(resolved,"boundaries_hz",field)==Status::ok);
    const auto values=std::get<std::span<const float>>(field);expect(values.data()==descriptor->default_array.data() && values.back()==16384);
}
void descriptor_owned_array_default_regression() {
    descriptor_boundary_array_default();
    expect(parameter_descriptor(NodeKind::ArrayFill,"frontend_declared"));
    expect(parameter_descriptor(NodeKind::ArrayFill,"values")->default_array.empty());
}
ConfiguredProcessingNode preparation_clock(NodeId id,const NumericContract& scalar) {
    auto node=source(id,scalar);node.kind="Number";
    node.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::uint64_t{48000}}}}};return node;
}
NumericProcessingGraph selector_clock_fixture() {
    const auto pcm=aligned_pcm_contract();auto rate=pcm;rate.shape={};rate.dtype=DType::UInt32;rate.unit=NumericUnit::Hertz;
    auto selector=heterogeneous_selector(pcm,rate);selector.id=5;std::swap(selector.outputs[0],selector.outputs[1]);
    NumericProcessingGraph graph;graph.graph_id="selector-preparation-lane";
    graph.nodes={preparation_clock(1,rate),source(2,pcm),source(3,pcm),preparation_clock(4,rate),selector,aligned_mixer(pcm,6)};
    graph.edges={{2,5,0,0},{1,5,0,1},{3,5,0,2},{4,5,0,3},{5,6,0,0},{5,6,0,1}};return graph;
}
void reject_streaming_selector_clock(NumericProcessingGraph& graph,const NumericDeviceCapabilities& device,
                                    std::span<std::size_t> workspace,ProcessingGraphAdmission& result) {
    const auto before=result;
    graph.nodes[0].kind="NetworkInput";graph.nodes[0].parameters=EndpointProcessingParameters{};
    const auto failure=admit_processing_graph(graph,limits,device,workspace,result);
    expect(failure.error==ProcessingError::IncompatiblePorts && failure.node==6);
    expect(result.node_count==before.node_count && result.peak_bytes==before.peak_bytes);
}
void selector_preparation_clock_regression() {
    auto graph=selector_clock_fixture();std::array<std::size_t,32> workspace{};ProcessingGraphAdmission result;
    NumericDeviceCapabilities device{nullptr,all_supported_query,{{65536,65536,65536,65536},65536,1048576}};
    expect(admit_processing_graph(graph,limits,device,workspace,result));reject_streaming_selector_clock(graph,device,workspace,result);
}
void number_preparation_literal_regression() {
    auto scalar=vector_contract(DType::Float64);scalar.shape={};scalar.coordinates={};scalar.unit=NumericUnit::Values;
    auto node=source(1,scalar);node.kind="Number";node.parameters=EndpointProcessingParameters{{{"value",NumericLiteral{std::int64_t{-1}}}}};
    NumericProcessingGraph graph;graph.graph_id="literal-preparation";graph.nodes={node};
    expect_preparation_literal(graph,1,0,NumericLiteral{std::int64_t{-1}});
    scalar.dtype=DType::UInt64;graph.nodes[0].outputs[0].contract=owned(scalar);
    std::get<EndpointProcessingParameters>(graph.nodes[0].parameters).values[0].value=NumericLiteral{std::uint64_t{UINT64_MAX}};
    expect_preparation_literal(graph,1,0,NumericLiteral{std::uint64_t{UINT64_MAX}});
    graph.nodes[0].enabled=false;expect_preparation_literal(graph,1,0,NumericLiteral{std::uint64_t{0}});
}
void matrix_row_contextual_bounds(ConfiguredProcessingNode& node) {
    expect(!processing_parameter_numeric_bounds(node,"row"));
    auto input=vector_contract(DType::Float32);input.shape={Rank::Matrix,{Axis::Time,Axis::Band},{ExtentBounds{0,3},ExtentBounds{4,4}}};input.coordinates={};
    node.inputs={port(0,PortDirection::Input,input)};
    const auto bounds=processing_parameter_numeric_bounds(node,"row");expect(bounds&&bounds->minimum==0&&bounds->maximum==2);
}
void unavailable_matrix_row_bounds(ConfiguredProcessingNode& node) {
    node.inputs[0].contract=owned(vector_contract(DType::Float32));expect(!processing_parameter_numeric_bounds(node,"row"));
    node.inputs[0].contract={};expect(!processing_parameter_numeric_bounds(node,"row"));
}
void generic_parameter_bounds(ConfiguredProcessingNode& node) {
    node.kind="MatrixFFT";const auto bounds=processing_parameter_numeric_bounds(node,"fft_size");
    expect(bounds&&bounds->minimum==2&&bounds->maximum==UINT32_MAX);expect(!processing_parameter_numeric_bounds(node,"unknown"));
}
void contextual_parameter_bounds_regression() {
    ConfiguredProcessingNode node;node.kind="MatrixRow";
    matrix_row_contextual_bounds(node);unavailable_matrix_row_bounds(node);generic_parameter_bounds(node);
}

void routing_descriptor_bounds() {
    const auto* outputs=parameter_descriptor(NodeKind::Switch,"outputs");
    expect(outputs&&outputs->minimum==min_route_outputs&&outputs->maximum==max_route_outputs&&outputs->default_number==min_route_outputs);
    const auto* lanes=parameter_descriptor(NodeKind::Switch,"channels_per_stream");
    expect(lanes&&lanes->minimum==1&&lanes->maximum==max_route_channels&&lanes->default_number==1);
    const auto* splitter=parameter_descriptor(NodeKind::Splitter,"outputs");
    expect(splitter&&splitter->minimum==min_route_outputs&&splitter->maximum==max_route_outputs);
}
void routing_selected_bounds_case(std::uint16_t outputs) {
    ConfiguredProcessingNode node;node.kind="Switch";node.parameters=RouteProcessingParameters{outputs};
    const auto bounds=processing_parameter_numeric_bounds(node,"selected");
    expect(bounds&&bounds->minimum==0&&bounds->maximum==outputs-1);
}
void routing_selected_invalid_context(std::uint16_t outputs) {
    ConfiguredProcessingNode node;node.kind="Switch";node.parameters=RouteProcessingParameters{outputs};
    expect(!processing_parameter_numeric_bounds(node,"selected"));
}
void routing_parameter_bounds_regression() {
    routing_descriptor_bounds();routing_selected_bounds_case(min_route_outputs);routing_selected_bounds_case(max_route_outputs);
    routing_selected_invalid_context(0);routing_selected_invalid_context(max_route_outputs+1);
}
void sample_clock_descriptor_bounds() {
    ConfiguredProcessingNode node;node.kind="SampleClock";
    const auto bounds=processing_parameter_numeric_bounds(node,"sample_rate");
    expect(bounds&&bounds->minimum==8000&&bounds->maximum==192000);
    const auto* descriptor=parameter_descriptor(NodeKind::SampleClock,"domain");expect(descriptor&&descriptor->default_text=="local");
}
void mixer_descriptor_bounds() {
    ConfiguredProcessingNode node;node.kind="Mixer";
    const auto bounds=processing_parameter_numeric_bounds(node,"fifo_milliseconds");
    expect(bounds&&bounds->minimum==1&&bounds->maximum==1000);
    expect(mixer_requires_nonnegative_weights(MixerMode::WeightedMean)&&mixer_requires_nonnegative_weights(MixerMode::WeightedRMS)
        &&!mixer_requires_nonnegative_weights(MixerMode::WeightedSum)&&!mixer_requires_nonnegative_weights(MixerMode::MaxAbsolute));
}
namespace {
void preparation_regressions() {
    contextual_parameter_bounds_regression();number_preparation_literal_regression();
    selector_preparation_clock_regression();direct_bypass_admission_regression();
    preparation_failure_reason_regression();descriptor_owned_array_default_regression();
    bypass_declared_sample_rate_regression();resampler_input_rate_binding_regression();
    unbound_generator_declared_clock_regression();explicit_preparation_diagnostic_regressions();
    explicit_preparation_provider_regressions();
}
void check_parameter_ports_unchanged(std::span<const ProcessingPort> actual,
                                     std::span<const ProcessingPort> baseline) {
    expect(actual.size()==baseline.size());
    if(actual.size()!=baseline.size())return;
    for(std::size_t i=0;i<actual.size();++i){
        expect(actual[i].id==baseline[i].id);
        expect(actual[i].direction==baseline[i].direction);
        expect(same_contract(actual[i].contract.view(),baseline[i].contract.view()));
    }
}
void window_auxiliary_port_limit_regression() {
    auto node=coefficient_generator_fixture(vector_contract(DType::Float32));
    const auto before=node;
    auto bounded=limits;
    bounded.max_ports_per_node=2;
    const auto allocations=allocation_probe::allocations;
    expect(attach_numeric_parameter_ports(node,bounded)==Status::exhausted);
    expect(allocation_probe::allocations==allocations);
    check_parameter_ports_unchanged(node.inputs,before.inputs);
    check_parameter_ports_unchanged(node.outputs,before.outputs);
}
void parameter_ports_late_failure_atomicity() {
    auto node=coefficient_generator_fixture(vector_contract(DType::Float32));
    node.kind="PcmResampler";
    const auto before=node;
    auto bounded=limits;
    bounded.max_owned_metadata_bytes=0;
    expect(attach_numeric_parameter_ports(node,bounded)!=Status::ok);
    check_parameter_ports_unchanged(node.inputs,before.inputs);
    check_parameter_ports_unchanged(node.outputs,before.outputs);
}
void window_auxiliary_ports_preserve_identity() {
    auto node=coefficient_generator_fixture(vector_contract(DType::Float32));
    const auto original=node.outputs.front();
    auto bounded=limits;
    bounded.max_ports_per_node=3;
    expect(attach_numeric_parameter_ports(node,bounded)==Status::ok);
    expect(node.outputs.size()==3);
    expect(node.outputs.front().id==original.id);
    expect(same_contract(node.outputs.front().contract.view(),original.contract.view()));
    const auto before=node;
    expect(attach_numeric_parameter_ports(node,bounded)==Status::ok);
    check_parameter_ports_unchanged(node.inputs,before.inputs);
    check_parameter_ports_unchanged(node.outputs,before.outputs);
}

void wled_streaming_scalar_contracts(ConfiguredProcessingNode& encoder) {
    const auto before=encoder.inputs[1].contract;
    auto data=before.view();auto& scalar=std::get<NumericContract>(data);
    scalar.clock.domain="other";encoder.inputs[1].contract=owned(scalar);
    expect(resolve(encoder).error==ProcessingError::IncompatiblePorts);
    encoder.inputs[1].contract=before;scalar=std::get<NumericContract>(before.view());
    scalar.dtype=DType::Float64;scalar.representation.alignment=alignof(double);
    encoder.inputs[1].contract=owned(scalar);expect(resolve(encoder));
}

void explicit_peak_contract_regression() {
    ConfiguredProcessingNode peak,encoder;
    expect(make_processing_node("SpectrumPeak",70,limits,peak));
    expect(make_processing_node("WLEDEncode",71,limits,encoder));
    expect(resolve(peak));expect(resolve(encoder));
    expect(numeric_parameter_key(peak.kind,spectrum_first_bin_input)=="first_bin");
    expect(numeric_parameter_key(peak.kind,frequency_step_input)=="bin_hz");
    expect(processing_ports_compatible(peak.outputs[0],encoder.inputs[1]));
    expect(processing_ports_compatible(peak.outputs[1],encoder.inputs[2]));
    wled_streaming_scalar_contracts(encoder);
}

void explicit_parameter_contract_regressions() {
    window_auxiliary_port_limit_regression();
    parameter_ports_late_failure_atomicity();
    window_auxiliary_ports_preserve_identity();
    explicit_peak_contract_regression();profile_reason_regressions();
}
void fixed_view_factory_regression() {
    constexpr std::array<std::pair<std::string_view,Rank>,14> views{{
        {"ScalarMetricsView",Rank::Scalar},{"ScalarSignedBarView",Rank::Scalar},
        {"VectorLineView",Rank::Vector},{"VectorPointView",Rank::Vector},
        {"VectorBarView",Rank::Vector},{"VectorPieView",Rank::Vector},
        {"MatrixImageView",Rank::Matrix},{"MatrixSurfaceView",Rank::Matrix},
        {"AudioLineView",Rank::Vector},{"AudioPointView",Rank::Vector},
        {"AudioBarView",Rank::Vector},{"AudioPieView",Rank::Vector},
        {"AudioImageView",Rank::Matrix},{"AudioSurfaceView",Rank::Matrix}}};
    for(const auto& [id,rank]:views) {
        const auto kind=node_kind(id);expect(kind.has_value());if(!kind)continue;
        expect(is_view(*kind) && node_role(*kind)==NodeRole::Sink);
        ConfiguredProcessingNode node;const auto created=make_processing_node(id,90,limits,node);
        expect(created);if(!created)continue;
        expect(data_inputs(node).size()==1 && node.outputs.empty());
        if(data_inputs(node).empty())continue;
        const auto contract=data_inputs(node).front().contract.view();
        const auto* numeric=std::get_if<NumericContract>(&contract);
        expect(numeric && numeric->shape.rank==rank);
        expect(resolve(node));
    }
}
void fixed_scalar_view_multi_input_regression() {
    for(const auto kind:{NodeKind::ScalarMetricsView,NodeKind::ScalarSignedBarView}) {
        ConfiguredProcessingNode node;const auto created=make_processing_node(kind,91,limits,node);
        expect(created);if(!created)continue;
        expect(is_scalar_view(kind));
        if(node.inputs.empty())continue;
        auto second=node.inputs.front();second.id=1;node.inputs.push_back(std::move(second));
        expect(resolve(node));
    }
}
void explicit_audio_axis_view_regression() {
    ConfiguredProcessingNode node;const auto created=make_processing_node("AudioLineView",92,limits,node);
    expect(created);if(!created)return;
    expect(numeric_parameter_key(node.kind,frequency_step_input)=="bin_hz");
    expect(std::any_of(node.inputs.begin(),node.inputs.end(),[](const auto& port){return port.id==frequency_step_input;}));
    expect(resolve(node));
    for(const auto* legacy:{"FFTView","MelView","WindowView","WindowedView","AudioView","SpectrogramView"})
        expect(node_kind(legacy).has_value());
}

void model_regressions() {
    sample_clock_descriptor_bounds();mixer_descriptor_bounds();
    routing_parameter_bounds_regression();
    parameter_map_identity_regression();switch_factory_contract_regression();projection_factory_grid_regression();
    profile_gain_admission_regressions();selector_reordered_scalar_regressions();input_selector_output_regressions();
    physical_configuration_matrix();aligned_ancestry_regressions();configuration_binding_regressions();
    lowering_interval_regressions();owner_tests();resolver_tests();reduction_history_tests();
    graph_tests();admission_boundary_tests();independent_findings_repairs();
    explicit_parameter_contract_regressions();
    fixed_view_factory_regression();fixed_scalar_view_multi_input_regression();explicit_audio_axis_view_regression();
}
}
int main() {
    preparation_regressions();model_regressions();
    std::printf("numeric processing contract failures=%d\n",failures);return failures?1:0;
}
