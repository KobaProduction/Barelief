// SPDX-License-Identifier: AGPL-3.0-or-later
#include <core_app/numeric_default_graph.h>
#include "example_builder.h"
#include <rhythm_graph_adapters/composition.h>
#include <rhythm_graph_adapters/descriptor_composition.h>
#include <rhythm_graph_adapters/event_composition.h>
#include <rhythm_graph_adapters/pcm_stage_composition.h>
#include <rhythm_reference_ports/reference_window.h>
#include <rhythm_reference_ports/context_beat_tracker.h>
#include <rhythm_reference_ports/adaptive_peak_picker.h>
#include <domain/numeric/array_math.h>
#include <domain/numeric/correlation_ports.h>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace {
using namespace domain;using namespace signal_contracts;
using namespace rhythm_graph_adapters;
using Maker=Status (*)(NodeId,std::string_view,std::span<const ProcessingPort>,TransformProcessingParameters,const FrameLimits&,std::size_t,ConfiguredProcessingNode&);
struct Link {NodeId node;PortId output=processing_output,input=processing_input;};
constexpr PortId real_part=0,imaginary_part=1;
void stage(examples::Builder& b,NodeId id,std::string_view kind,Maker make,TransformProcessingParameters parameters,std::initializer_list<Link> links) {
    std::vector<ProcessingPort> inputs;inputs.reserve(links.size());
    for(const auto& link:links)inputs.push_back({link.input,PortDirection::Input,b.output(link.node,link.output)});
    ConfiguredProcessingNode node;
    if(make(id,kind,inputs,std::move(parameters),b.limits.frame,b.limits.max_owned_metadata_bytes,node)!=Status::ok)
        throw std::runtime_error("Aubio graph stage: "+std::string(kind));
    b.graph.nodes.push_back(std::move(node));
    for(const auto& link:links)b.connect(link.node,id,link.output,link.input);
}
NumericArray indices(std::size_t size,std::size_t shift=0) {
    NumericArray result;result.values.reserve(size);
    for(std::size_t i=0;i<size;++i)result.values.push_back(float((i+shift)%size));
    return result;
}
NumericArray source_window() {
    NumericArray values;values.values.resize(1024);
    if(!rhythm_reference_ports::analysis_window(values.values,rhythm_reference_ports::AnalysisWindowShape::PeriodicHann))throw std::runtime_error("Aubio window");
    return values;
}
NumericArray reverse_order(std::size_t count) {
    NumericArray result;result.values.reserve(count);
    for(std::size_t i=count;i>0;--i)result.values.push_back(float(i-1));
    return result;
}
NumericArray source_coefficients(std::span<const float> values) {
    NumericArray result;result.values.assign(values.begin(),values.end());return result;
}
TransformProcessingParameters source_smoothing() {
    using namespace rhythm_reference_ports;
    return examples::transform({{"numerator",source_coefficients(adaptive_peak_numerator)},
        {"denominator",source_coefficients(adaptive_peak_denominator)},
        {"form",NumericLiteral{std::uint64_t{1}}},{"subnormal",NumericLiteral{std::uint64_t{0}}},
        {"state_policy",NumericLiteral{std::uint64_t{1}}},
        {"input_zero_threshold",NumericLiteral{std::nextafter(2e-42f,std::numeric_limits<float>::infinity())}}});
}
void smooth_descriptor(examples::Builder& b,NodeId descriptor,NodeId first) {
    b.chain(first,"Window",descriptor,examples::transform({{"size",NumericLiteral{std::uint64_t{7}}}}),descriptor_ports::value);
    b.chain(first+1,"RecursiveFilter",first,source_smoothing());
    b.chain(first+2,"IndexExtract",first+1,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",reverse_order(7)}}));
    b.chain(first+3,"RecursiveFilter",first+2,source_smoothing());
    b.chain(first+4,"IndexExtract",first+3,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",reverse_order(7)}}));
}
void frontend(examples::Builder& b) {
    b.add(1,"AudioSource",EndpointProcessingParameters{{{"source",NumericLiteral{std::uint64_t{0}}},{"output_count",NumericLiteral{std::uint64_t{1}}}}});
    b.chain(2,"PcmResampler",1,examples::transform({{"mode",std::string{"existing_fir"}},{"output_rate",NumericLiteral{std::uint64_t{44100}}}}));
    b.connect(1,2,sample_rate_output,input_sample_rate_input);
    b.chain(3,"FrameBuffer",2,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"hop_samples",NumericLiteral{std::uint64_t{512}}}}));
    b.chain(4,"Window",3,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}}}));
    b.chain(5,"ArrayLength",4);
    b.chain(6,"ArrayFill",5,examples::transform({{"size",NumericLiteral{std::uint64_t{1024}}},{"values",source_window()}}));
    b.add(7,"ArrayMultiply");b.connect(4,7,processing_output,PortId(BinaryInput::Left));
    b.connect(6,7,processing_output,PortId(BinaryInput::Right));b.derive(7);
    b.chain(8,"IndexExtract",7,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",indices(1024,512)}}));
    b.chain(10,"RealFourier",8,examples::transform({{"fft_size",NumericLiteral{std::uint64_t{1024}}},
        {"backend_id",std::string{"aubio_ooura_fft8g_float32"}}}));
}
void polar(examples::Builder& b) {
    b.add(11,"ComplexPolar",examples::transform({
        {"fft_size",NumericLiteral{std::uint64_t{1024}}},{"first_bin",NumericLiteral{std::uint64_t{0}}},
        {"positive_half",true},{"magnitude_norm",std::string{"squared_norm"}},
        {"zero_phase",std::string{"atan2"}},{"real_endpoints",true}}));
    b.connect(10,11,real_part,real_part);b.connect(10,11,imaginary_part,imaginary_part);b.derive(11);
}
void onset(examples::Builder& b) {
    using namespace rhythm_graph_adapters;
    b.chain(20,"LogCompression",11,examples::transform({{"base",NumericLiteral{std::uint64_t{1}}},
        {"multiply",NumericLiteral{1.f}},{"add",NumericLiteral{1.f}},{"signed_input",false}}));
    stage(b,21,"OnsetDescriptor",make_descriptor_operation_node,examples::transform({{"method",NumericLiteral{std::uint64_t{1}}}}),{{20,processing_output,descriptor_ports::magnitude}});
    smooth_descriptor(b,21,60);
    stage(b,22,"AdaptivePeakPicker",make_event_operation_node,examples::transform({{"threshold",NumericLiteral{.058f}}}),{{64,processing_output,peak_picker_ports::smoothed_history}});
    stage(b,23,"PcmSilence",make_event_operation_node,examples::transform({{"threshold_db",NumericLiteral{-70.f}}}),{{3,processing_output,silence_ports::pcm}});
    stage(b,24,"OnsetDecision",make_event_operation_node,examples::transform({{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"delay_samples",NumericLiteral{std::uint64_t{2201}}},{"minimum_interval_samples",NumericLiteral{std::uint64_t{2205}}}}),{{22,peak_picker_ports::position,onset_decision_ports::peak_position},
        {23,silence_ports::silent,onset_decision_ports::silence}});
}
NumericArray recency_coefficients() {
    NumericArray result;result.values.resize(512);
    if(!rhythm_reference_ports::context_detection_weights({},result.values))throw std::runtime_error("Aubio context weights");
    return result;
}
void context_frontend(examples::Builder& b) {
    // Exclude this frame: source updates a due block before consuming its ODF.
    b.chain(50,"Window",31,examples::transform({{"size",NumericLiteral{std::uint64_t{513}}}}),peak_picker_ports::thresholded);
    b.chain(51,"IndexExtract",50,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",indices(512)}}));
    b.chain(52,"ArrayLength",51);
    b.chain(53,"ArrayFill",52,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}},{"values",recency_coefficients()}}));
    b.add(54,"ArrayMultiply");b.connect(51,54,processing_output,PortId(BinaryInput::Left));
    b.connect(53,54,processing_output,PortId(BinaryInput::Right));b.derive(54);
    b.chain(55,"IndexExtract",54,examples::transform({{"mode",NumericLiteral{std::uint64_t{1}}},{"indices",reverse_order(512)}}));
    b.add(56,"DirectCorrelation",examples::transform({{"lags",NumericLiteral{std::uint64_t{512}}},{"normalization",NumericLiteral{std::uint64_t{1}}}}));
    b.connect(51,56,processing_output,PortId(CorrelationInput::Reference));
    b.connect(51,56,processing_output,PortId(CorrelationInput::Compared));b.derive(56);
}
void tempo(examples::Builder& b) {
    using namespace rhythm_graph_adapters;
    stage(b,30,"OnsetDescriptor",make_descriptor_operation_node,examples::transform({{"method",NumericLiteral{std::uint64_t{7}}}}),{{11,processing_output,descriptor_ports::magnitude}});
    smooth_descriptor(b,30,70);
    stage(b,31,"AdaptivePeakPicker",make_event_operation_node,examples::transform({{"threshold",NumericLiteral{.3f}}}),{{74,processing_output,peak_picker_ports::smoothed_history}});
    stage(b,32,"PcmSilence",make_event_operation_node,examples::transform({{"threshold_db",NumericLiteral{-90.f}}}),{{3,processing_output,silence_ports::pcm}});
    context_frontend(b);
    stage(b,33,"ContextBeatSequence",make_pcm_stage_node,examples::transform({{"hop_samples",NumericLiteral{std::uint64_t{512}}},
        {"frame_rate",NumericLiteral{44100.f/512.f}},{"tracker_window",NumericLiteral{std::uint64_t{512}}},
        {"delay_samples",NumericLiteral{std::int64_t{0}}},{"tatums_per_beat",NumericLiteral{std::uint64_t{4}}},{"poll_tatum",true}}),{{51,processing_output,context_sequence_ports::history_input},
        {32,silence_ports::silent,context_sequence_ports::silence},{55,processing_output,context_sequence_ports::reversed_weighted_history},
        {56,processing_output,context_sequence_ports::autocorrelation_input}});
}
void view(examples::Builder& b,NodeId id,NodeId source,PortId port=processing_output,bool scalar=false) {
    b.chain(id,scalar?"ScalarMetricsView":"VectorLineView",source,{},port);
}
void views(examples::Builder& b) {
    b.chain(40,"Window",21,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}));view(b,104,40);
    b.chain(41,"Window",31,examples::transform({{"size",NumericLiteral{std::uint64_t{512}}}}),peak_picker_ports::thresholded);view(b,105,41);
    view(b,107,24,onset_decision_ports::detected,true);
    view(b,106,24,onset_decision_ports::last_sample,true);
    view(b,108,33,context_sequence_ports::bpm,true);
    view(b,115,33,context_sequence_ports::scheduled,true);
    view(b,109,33,context_sequence_ports::beat_sample,true);
    view(b,113,33,context_sequence_ports::tatum_event,true);
    view(b,114,33,context_sequence_ports::tatum_sample,true);
}
void describe(NumericExampleGraph& graph) {
    graph.node_names={{1,"Audio input"},{2,"Resample for analysis"},{3,"Analysis step"},{4,"Fourier window"},
        {5,"Fourier window length"},{6,"Hann coefficients"},{7,"Apply Hann window"},{8,"Rotate analysis window"},
        {10,"Fourier transform"},{11,"Spectrum magnitude and phase"},
        {20,"Compress onset spectrum"},{21,"Onset high-frequency content"},{22,"Find onset peaks"},
        {23,"Onset silence gate"},{24,"Detect onsets"},{30,"Tempo spectral flux"},{31,"Find tempo peaks"},
        {32,"Tempo silence gate"},{33,"Track beat context"},{40,"Onset history"},{41,"Thresholded tempo history"},
        {50,"Tempo context window"},{51,"Exclude current onset"},{52,"Tempo context length"},
        {53,"Recency weights"},{54,"Weight recent onsets"},{55,"Reverse weighted history"},
        {56,"Onset autocorrelation"},
        {60,"Onset smoothing window"},{61,"Onset forward smoothing"},{62,"Reverse onset history"},
        {63,"Onset reverse smoothing"},{64,"Restore onset order"},
        {70,"Tempo smoothing window"},{71,"Tempo forward smoothing"},{72,"Reverse tempo history"},
        {73,"Tempo reverse smoothing"},{74,"Restore tempo order"},
        {104,"Aubio HFC history"},{105,"Aubio thresholded flux history"},
        {107,"Aubio onset detected"},{106,"Aubio last onset sample"},{108,"Aubio tempo, BPM"},
        {115,"Aubio beat scheduled"},{109,"Aubio last beat sample"},
        {113,"Aubio tatum event code"},{114,"Aubio last tatum sample"}};
    for(std::size_t i=0;i<graph.graph.nodes.size();++i)graph.positions.push_back({graph.graph.nodes[i].id,double(i%8)*440,double(i/8)*420});
}
}
NumericExampleGraph numeric_aubio_reference_example() {
    examples::Builder b({44100,1024,512,512});b.graph.graph_id="aubio-reference";
    frontend(b);polar(b);onset(b);tempo(b);views(b);
    NumericExampleGraph result{"aubio-reference","Aubio: default onset and tempo",b.finish()};describe(result);return result;
}
