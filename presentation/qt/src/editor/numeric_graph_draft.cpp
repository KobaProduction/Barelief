#include "numeric_graph_draft.h"
#include <domain/graph/node_factory.h>
#include <algorithm>
#include <variant>
namespace numeric_editor {
bool music_operation(std::string_view kind) {
    if(kind=="RhythmAnalyzer" || kind=="RhythmFeatures")return true;
    return kind=="BroadbandRow" || kind=="NoteSpectrum" || kind=="ArrayConcat" || kind=="MusicalSpectrogram" || kind=="ArrayFill" || kind=="ArrayMultiply" || kind=="AmplitudePhase" || kind=="ComplexPolar" || kind=="Fourier" || kind=="RealFourier" || kind=="RFFT" || kind=="SpectrumProjection" || kind=="FrequencyNormalizer" ||
        kind=="BroadbandFeatures" || kind=="NoteSalience" || kind=="SpectralQuantize";
}
bool draft_view(std::string_view kind) {
    const auto k=domain::node_kind(kind);
    return k && domain::node_role(*k)==domain::NodeRole::Sink && domain::is_view(*k);
}
bool draft_view_accepts(std::string_view kind,const signal_contracts::NumericContract& c) {
    const auto matrix=kind=="MatrixImageView" || kind=="MatrixSurfaceView" ||
        kind=="AudioImageView" || kind=="AudioSurfaceView" || kind=="SpectrogramView";
    const auto scalar=kind=="ScalarView" || kind=="ScalarMetricsView" || kind=="ScalarSignedBarView";
    const auto rank=scalar?signal_contracts::Rank::Scalar:
        matrix?signal_contracts::Rank::Matrix:signal_contracts::Rank::Vector;
    return draft_view(kind) && (c.shape.rank==rank || (kind=="FFTView" && c.shape.rank==signal_contracts::Rank::Scalar && c.channel_order.size()==1));
}

bool matrix_operation(std::string_view kind){return kind.starts_with("Matrix");}
namespace {
bool adopt(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return bool(domain::configure_processing_input(node,input,limits()));}
}
bool adopt_music_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return adopt(node,input);}
bool adopt_sample_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return adopt(node,input);}
bool adopt_history_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return adopt(node,input);}
bool adopt_matrix_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return adopt(node,input);}
bool adopt_extract_input(domain::ConfiguredProcessingNode& node,const signal_contracts::NumericContract& input){return adopt(node,input);}
bool adapt_mel_parameters(domain::ConfiguredProcessingNode& node){return bool(domain::derive_processing_node_contracts(node,limits()));}
bool propagate_clock(domain::NumericProcessingGraph& graph,domain::NodeId root,const NodeCheck&) {
    domain::NumericProcessingGraph evaluated,executable;
    if(!domain::resolve_numeric_parameter_bindings(graph,limits(),evaluated,executable,root))return false;
    graph=std::move(evaluated);return true;
}
namespace {
domain::ConfiguredProcessingNode* find_destination(domain::NumericProcessingGraph& graph,const domain::Edge& edge) {
    auto to=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==edge.to;});
    return to==graph.nodes.end()?nullptr:&*to;
}
bool same_connected_numeric_dtype(const domain::ProcessingPort& source,const domain::ProcessingPort& target) {
    const auto from=source.contract.view(),to=target.contract.view();
    const auto* input=std::get_if<signal_contracts::NumericContract>(&from);
    const auto* previous=std::get_if<signal_contracts::NumericContract>(&to);
    return input && previous && input->dtype==previous->dtype;
}
bool adopt_connected_input(const domain::NumericProcessingGraph& graph,const domain::Edge& edge,domain::ConfiguredProcessingNode& target,bool fresh) {
    const auto from=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==edge.from;});
    auto in=std::find_if(target.inputs.begin(),target.inputs.end(),[&](const auto& port){return port.id==edge.input_port;});
    if(from==graph.nodes.end() || in==target.inputs.end())return false;
    const auto out=std::find_if(from->outputs.begin(),from->outputs.end(),[&](const auto& port){return port.id==edge.output_port;});
    if(out==from->outputs.end())return false;
    if(!fresh && !same_connected_numeric_dtype(*out,*in))return true;
    in->contract=out->contract;return true;
}
bool adopt_connected_inputs(const domain::NumericProcessingGraph& graph,domain::ConfiguredProcessingNode& target,const domain::Edge& added) {
    for(const auto& edge:graph.edges)if(edge.to==target.id)
        if(!adopt_connected_input(graph,edge,target,&edge==&added))return false;
    return true;
}

bool connect_candidate(const domain::Edge& edge,domain::NumericProcessingGraph& candidate,domain::NodeId& destination) {
    auto* to=find_destination(candidate,edge);if(!to)return false;
    candidate.edges.push_back(edge);
    // Retained same-dtype draft inputs also receive their directly connected
    // contracts. Existing conversions and complete preparation remain authoritative.
    if(!adopt_connected_inputs(candidate,*to,candidate.edges.back()))return false;
    if(domain::numeric_parameter_key(to->kind,edge.input_port).empty() && !domain::derive_processing_node_contracts(*to,limits()))return false;
    destination=to->id;return true;
}
}
bool adapt_connection_clock(const domain::Edge& edge,domain::NumericProcessingGraph& graph,const NodeCheck& check) {
    auto candidate=graph;
    domain::NodeId destination{};
    if(!connect_candidate(edge,candidate,destination))return false;
    if(!propagate_clock(candidate,destination,check))return false;
    graph=std::move(candidate);return true;
}
}
