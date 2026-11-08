// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_btrack_reference_graph.h>
#include <core_app/numeric_default_graph.h>
#include <rhythm_graph_adapters/btrack_composition.h>
#include <rhythm_graph_adapters/btrack_ports.h>
#include <domain/numeric/array_math.h>
#include <domain/graph/window_parameters.h>
#include "example_builder.h"
#include <stdexcept>

namespace {
using namespace domain;
using signal_contracts::NumericLiteral;
namespace ports=rhythm_graph_adapters::btrack_ports;
constexpr PortId real_plane=0,imaginary_plane=1,left_operand=PortId(BinaryInput::Left),right_operand=PortId(BinaryInput::Right),value_output=0;
TransformProcessingParameters source_parameters() {
    return examples::transform({{"sample_rate",NumericLiteral{std::uint64_t{44100}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"frame_samples",NumericLiteral{std::uint64_t{1024}}},{"history_size",NumericLiteral{std::uint64_t{512}}},
        {"alpha",NumericLiteral{.9f}},{"tightness",NumericLiteral{5.f}}});
}
void stage(examples::Builder& b,NodeId id,std::string_view kind,NodeId first,PortId output=value_output,
    NodeId second=0,PortId second_output=value_output,std::size_t size=512,PortId second_input=ports::port(ports::OnsetInput::Imaginary)) {
    std::vector<ProcessingPort> inputs{{ports::values_input,PortDirection::Input,b.output(first,output)}};
    if(second)inputs.push_back({second_input,PortDirection::Input,b.output(second,second_output)});
    ConfiguredProcessingNode node;auto parameters=source_parameters();
    parameters.values.push_back({"size",NumericLiteral{std::uint64_t(size)}});
    if(rhythm_graph_adapters::make_btrack_operation_node(id,kind,inputs,parameters,b.limits.frame,
        b.limits.max_owned_metadata_bytes,node)!=signal_contracts::Status::ok)throw std::runtime_error("BTrack reference stage contract");
    b.graph.nodes.push_back(std::move(node));b.connect(first,id,output);
    if(second)b.connect(second,id,second_output,second_input);
}
void arithmetic(examples::Builder& b,NodeId id,NodeId left,NodeId right,ArrayMathOperation operation,
    PortId left_output=value_output,PortId right_output=value_output,PortId frame_input=left_operand) {
    b.add(id,"ArrayMultiply",examples::transform({{"operation",std::string(array_math_name(operation))},{"frame_input",NumericLiteral{std::uint64_t(frame_input)}}}));
    b.connect(left,id,left_output,left_operand);b.connect(right,id,right_output,right_operand);b.derive(id);
}
void float32(examples::Builder& b,NodeId id,NodeId input) {
    ConverterProcessingParameters parameters;parameters.policy.precision_loss=signal_contracts::PrecisionLoss::AllowDeclared;
    b.chain(id,"Converter",input,parameters);
}
NumericArray indices(std::size_t count,std::size_t rotation=0) {
    NumericArray result;result.values.resize(count);
    for(std::size_t i=0;i<count;++i)result.values[i]=float((i+rotation)%count);
    return result;
}
void window_product(examples::Builder& b) {
    b.chain(20,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}}}));
    b.chain(21,"ArrayLength",20);
    b.chain(22,"WindowFunction",21,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}},
        {"function",std::string{"hann"}},{"periodic",false},{"precision",NumericLiteral{std::uint64_t{1}}},
        {"evaluation",NumericLiteral{std::uint64_t(signal_contracts::WindowEvaluation::Direct)}},{"pi",NumericLiteral{3.14159265358979}}}));
    arithmetic(b,23,20,22,ArrayMathOperation::Multiply);float32(b,27,23);
}
void frontend(examples::Builder& b,std::uint32_t input_rate) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},
        {"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.chain(2,"PcmResampler",1,examples::transform({{"input_rate",NumericLiteral{std::uint64_t(input_rate)}},
        {"output_rate",NumericLiteral{std::uint64_t{44100}}},{"mode",std::string{"existing_fir"}}}));
    b.connect(1,2,sample_rate_output,input_sample_rate_input);
    b.chain(3,"FrameBuffer",2,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    window_product(b);
    b.chain(24,"IndexExtract",27,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",indices(1024,512)}}));
    b.chain(25,"Fourier",24,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"backend_id",std::string{"kiss_fft130_float32"}}}));
    b.node(25).inputs[imaginary_plane].contract=b.output(24);b.connect(21,25,value_output,window_size_input);
}
void polar(examples::Builder& b,NodeId id,NodeId transform,std::string norm,bool endpoints=false) {
    b.add(id,"ComplexPolar",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"first_bin",NumericLiteral{std::uint64_t{0}}},{"positive_half",false},{"real_endpoints",endpoints},
        {"magnitude_norm",std::move(norm)},{"zero_phase",std::string{"atan2"}}}));
    b.connect(transform,id,real_plane,real_plane);b.connect(transform,id,imaginary_plane,imaginary_plane);b.derive(id);
}
void onset_history(examples::Builder& b) {
    polar(b,26,25,"squared_norm",true);
    stage(b,30,"BTrackOnset",25,real_plane,25,imaginary_plane);
    b.chain(52,"AbsoluteValue",30);
    b.chain(53,"NumericMap",52,examples::transform({{"offset",NumericLiteral{.0001}},
        {"output_dtype",NumericLiteral{std::uint64_t{1}}}}));float32(b,29,53);
    stage(b,31,"BTrackHistory",29);
    stage(b,32,"BTrackHistoryResample",31,ports::port(ports::HistoryOutput::OnsetWindow));
}
void autocorrelation_transform(examples::Builder& b) {
    stage(b,70,"BTrackAdaptiveThreshold",32);
    b.chain(71,"ArrayLength",70);b.chain(72,"ArrayFill",71,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"value",NumericLiteral{0.f}}}));
    b.add(73,"ArrayConcat");b.connect(70,73,value_output,left_operand);b.connect(72,73,value_output,right_operand);b.derive(73);
    b.chain(74,"Fourier",73,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}}}));
    b.node(74).inputs[imaginary_plane].contract=b.output(73);
    arithmetic(b,75,74,74,ArrayMathOperation::Multiply,real_plane,real_plane,right_operand);
    arithmetic(b,76,74,74,ArrayMathOperation::Multiply,imaginary_plane,imaginary_plane,right_operand);
    arithmetic(b,77,75,76,ArrayMathOperation::Add,value_output,value_output,right_operand);
    b.chain(78,"ArrayLength",77);b.chain(79,"ArrayFill",78,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}},{"value",NumericLiteral{0.f}}}));
    b.add(80,"InverseFourier",examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}}}));
    b.connect(77,80,value_output,real_plane);b.connect(79,80,value_output,imaginary_plane);b.derive(80);
}
void autocorrelation_balance(examples::Builder& b) {
    polar(b,81,80,"hypot");
    b.chain(82,"IndexExtract",81,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",indices(512)}}));
    NumericArray denominators;denominators.values.resize(512);
    for(std::size_t lag=0;lag<512;++lag)denominators.values[lag]=float(512-lag);
    b.chain(86,"ArrayLength",82);b.chain(87,"ArrayFill",86,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"values",std::move(denominators)}}));
    arithmetic(b,83,82,87,ArrayMathOperation::Divide);
}
void tracking(examples::Builder& b) {
    onset_history(b);autocorrelation_transform(b);autocorrelation_balance(b);
    stage(b,84,"BTrackCombResponse",83);stage(b,85,"BTrackAdaptiveThreshold",84,value_output,0,value_output,128);
    stage(b,33,"BTrackCombObservation",85);
    stage(b,34,"BTrackBeatDecision",31,ports::port(ports::HistoryOutput::NewestOnset),33,value_output,512,ports::port(ports::DecisionInput::TempoObservations));
}
void views(examples::Builder& b) {
    b.chain(48,"ScalarMetricsView",34,{},ports::port(ports::DecisionOutput::TempoBpm));
    b.chain(35,"Window",34,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}),ports::port(ports::DecisionOutput::BeatFlag));
    b.chain(50,"VectorLineView",35);b.chain(51,"ScalarMetricsView",30);
}
void layout(NumericExampleGraph& graph) {
    std::size_t column=0;
    for(const auto& node:graph.graph.nodes)graph.positions.push_back({node.id,double(column++%12)*450.,double((column-1)/12)*600.});
    graph.node_names={{48,"Tempo, BPM"},{50,"Beat flag history (0/1)"},{51,"Onset strength (ODF)"}};
    graph.scalar_names={{48,{"Tempo, BPM"}},{51,{"Onset strength (ODF)"}}};
}
}
NumericExampleGraph numeric_btrack_reference_example(std::uint32_t input_rate) {
    if(input_rate<8000 || input_rate>96000)throw std::invalid_argument("BTrack input rate invalid");
    examples::Builder b({input_rate,1024,512,400});b.graph.graph_id="reference-btrack";
    frontend(b,input_rate);tracking(b);views(b);
    NumericExampleGraph graph{"reference-btrack","Rhythm: BTrack reference pipeline",b.finish()};layout(graph);return graph;
}
