#include <desktop/numeric_source_format.h>
#include <desktop/numeric_project_document.h>
#include <domain/graph/node_factory.h>
#include <core_app/numeric_default_graph.h>
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
using namespace domain;
using namespace signal_contracts;
constexpr ProcessingAdmissionLimits limits{128,1024,128,128,1048576,16777216,
    {64,16777216,16777216,16777216,1024,16777216}};
void require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
ConfiguredProcessingNode make(std::string_view kind,NodeId id){
    ConfiguredProcessingNode result;require(bool(make_processing_node(kind,id,limits,result)),"factory fixture");return result;
}
const ConfiguredProcessingNode& find(const NumericProcessingGraph& graph,NodeId id){
    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[=](const auto& value){return value.id==id;});
    require(node!=graph.nodes.end(),"fixture node");return *node;
}
void unchanged_node(const ConfiguredProcessingNode& before,const ConfiguredProcessingNode& after){
    require(before.kind==after.kind && before.clock_node==after.clock_node && before.enabled==after.enabled,"source discovery changed unrelated node identity");
    require(same_processing_parameters(before.parameters,after.parameters),"source discovery retuned unrelated parameters");
    require(before.outputs.size()==after.outputs.size(),"source discovery changed unrelated ports");
    for(std::size_t i=0;i<before.outputs.size();++i)
        require(same_contract(before.outputs[i].contract.view(),after.outputs[i].contract.view()),"source discovery changed unrelated output contract");
}
NumericProcessingGraph independent_sources(){
    auto graph=numeric_generator_graph();
    std::erase_if(graph.nodes,[](const auto& node){return node.id!=1 && node.id!=16;});
    graph.edges.clear();graph.execution_interval={1,119};graph.nodes.push_back(make("AudioSource",13));
    return graph;
}
void no_implicit_retuning(){
    auto graph=independent_sources();const auto before=graph;QString error;
    require(desktop::resolve_numeric_source_format(graph,13,44100,2,limits,&error),"native reconciliation of independent capture");
    require(graph.execution_interval==before.execution_interval,"native Fs rounded the analysis cadence");
    require(graph.configuration_bindings==before.configuration_bindings,"native Fs rewrote clock bindings");
    unchanged_node(find(before,1),find(graph,1));unchanged_node(find(before,16),find(graph,16));
    const auto outputs=data_outputs(find(graph,13));require(outputs.size()==2,"native data output count excludes auxiliary Fs");
    for(const auto& port:outputs)require(std::get<NumericContract>(port.contract.view()).clock.sample_rate==44100,"native capture Fs declared");
    const auto& all=find(graph,13).outputs;
    require(std::any_of(all.begin(),all.end(),[](const auto& port){return port.id==sample_rate_output;}),"native capture retains explicit Fs output");
}
void declared_source_rate_bounds(){
    auto graph=independent_sources();
    QString error;
    require(desktop::resolve_numeric_source_format(graph,13,1000,1,limits,&error),"native capture must use AudioSource bounds rather than SampleClock bounds");
    const auto outputs=data_outputs(find(graph,13));
    require(outputs.size()==1,"low-rate native capture remains mono");
    require(std::get<NumericContract>(outputs.front().contract.view()).clock.sample_rate==1000,"native rate is declared exactly");
    unchanged_node(find(independent_sources(),1),find(graph,1));
}
void incompatible_route_is_atomic(){
    auto graph=independent_sources();graph.nodes.push_back(make("Number",30));graph.nodes.push_back(make("Window",31));
    graph.edges.push_back({30,31,0,0});
    QByteArray before,after;require(bool(desktop::encode_numeric_graph(graph,before)),"serialize incompatible draft before discovery");
    QString error;require(!desktop::resolve_numeric_source_format(graph,13,44100,1,limits,&error),"incompatible scalar-to-window route must refuse reconciliation");
    require(!error.isEmpty(),"incompatible route gives reason");
    require(desktop::encode_numeric_graph(graph,after) && before==after,"refused native format changed the draft");
}
NumericProcessingGraph connected_native_outputs() {
    auto graph=independent_sources();QString error;
    require(desktop::resolve_numeric_source_format(graph,13,48000,2,limits,&error),"two-channel native declaration");
    auto sink=make("NetworkOutput",40);
    require(bool(configure_processing_input(sink,data_outputs(find(graph,13))[1].contract.view(),limits)),"sink contract");
    graph.nodes.push_back(std::move(sink));graph.edges.push_back({13,40,1,0});
    return graph;
}
void connected_native_outputs_unchanged(const NumericProcessingGraph& graph,const QByteArray& before) {
    QByteArray after;
    require(data_outputs(find(graph,13)).size()==2,"refused native format preserves data outputs");
    require(graph.edges.size()==1 && graph.edges.front().output_port==1,"refused native format preserves the connected edge");
    require(desktop::encode_numeric_graph(graph,after) && before==after,"refused native output removal preserves the document");
    unchanged_node(find(independent_sources(),1),find(graph,1));
}
void removed_channel_refuses_atomically(){
    auto graph=connected_native_outputs();
    QString error;
    QByteArray before;
    require(bool(desktop::encode_numeric_graph(graph,before)),"serialize connected native source");
    desktop::NumericSourceFormatDiagnostic diagnostic;
    require(!desktop::resolve_numeric_source_format(graph,13,48000,1,limits,&error,&diagnostic),"connected native output removal must refuse");
    require(diagnostic.error==desktop::NumericSourceFormatError::ConnectedOutputUnavailable && diagnostic.node==13 && diagnostic.port==PortId{1},"native failure identifies the unavailable connected output");
    connected_native_outputs_unchanged(graph,before);
}
void restrictive_profile_is_atomic(const ProcessingAdmissionLimits& admission) {
    auto graph=independent_sources();
    QByteArray before,after;
    require(bool(desktop::encode_numeric_graph(graph,before)),"serialize profile refusal fixture");
    QString error;
    desktop::NumericSourceFormatDiagnostic diagnostic;
    require(!desktop::resolve_numeric_source_format(graph,13,44100,2,admission,&error,&diagnostic),"native discovery must obey caller admission");
    require(diagnostic.error==desktop::NumericSourceFormatError::AdmissionLimit && diagnostic.node==13 && !diagnostic.port,"profile refusal has typed source diagnostic");
    require(!error.isEmpty(),"profile refusal has reason");
    require(desktop::encode_numeric_graph(graph,after) && before==after,"profile refusal preserves graph atomically");
}
void owning_profile_limits() {
    auto bounded=limits;
    bounded.max_nodes=1;
    restrictive_profile_is_atomic(bounded);
    bounded=limits;bounded.max_owned_metadata_bytes=1;
    restrictive_profile_is_atomic(bounded);
    bounded=limits;bounded.max_ports_per_node=2;
    restrictive_profile_is_atomic(bounded);
}

}
int main(){
    try{no_implicit_retuning();declared_source_rate_bounds();incompatible_route_is_atomic();removed_channel_refuses_atomically();owning_profile_limits();return 0;}
    catch(const std::exception& error){std::fprintf(stderr,"FAIL native format: %s\n",error.what());return 1;}
}
