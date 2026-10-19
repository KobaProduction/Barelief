#include "numeric_graph_draft.h"
#include <domain/graph/node_factory.h>
#include <algorithm>
namespace numeric_editor {
bool music_operation(std::string_view kind) {
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
bool connect_candidate(const domain::Edge& edge,domain::NumericProcessingGraph& candidate,domain::NodeId& destination) {
    auto from=std::find_if(candidate.nodes.begin(),candidate.nodes.end(),[&](const auto& n){return n.id==edge.from;});
    auto* to=find_destination(candidate,edge);
    if(from==candidate.nodes.end() || !to)return false;
    const auto out=std::find_if(from->outputs.begin(),from->outputs.end(),[&](const auto& p){return p.id==edge.output_port;});
    auto in=std::find_if(to->inputs.begin(),to->inputs.end(),[&](const auto& p){return p.id==edge.input_port;});
    if(out==from->outputs.end() || in==to->inputs.end())return false;
    in->contract=out->contract;candidate.edges.push_back(edge);
    if(domain::numeric_parameter_key(to->kind,edge.input_port).empty() && !domain::derive_processing_node_contracts(*to,limits()))return false;
    destination=to->id;
    return true;
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
