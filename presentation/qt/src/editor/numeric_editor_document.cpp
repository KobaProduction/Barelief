#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "barelief/ui_text.h"
#include <desktop/numeric_project_document.h>
#include <desktop/numeric_source_format.h>
#include <QJsonDocument>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QJsonArray>
#include <algorithm>
#include <limits>
#include <numeric>
#include <cmath>
#include <set>
#include <tuple>
#include <type_traits>
#include <QTreeWidget>
#include <QScopedValueRollback>

namespace {
auto edge_key(QtNodes::ConnectionId e){return std::make_tuple(e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex);}
bool unique_sample_clock_bindings(const domain::NumericProcessingGraph& graph) {
    for(const auto& binding:graph.configuration_bindings)
        if(binding.input==domain::ConfigurationRole::SampleClock &&
           std::count_if(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),[&](const auto& other){
               return other.to==binding.to && other.input==binding.input;
           })!=1)return false;
    return true;
}
}
void NumericEditorGraphModel::load(const QJsonObject& json){
    if(json.value("version").toInt()!=5 || json.value("processing_version").toInt()!=4)return;
    domain::NumericProcessingGraph decoded;
    if(!desktop::decode_numeric_editor_graph(QJsonDocument(json).toJson(QJsonDocument::Compact),decoded))return;
    if(!unique_sample_clock_bindings(decoded))return;
    _execution_interval=decoded.execution_interval;
    _graph_id=json.value("graph_id").toString("main").toStdString();_edge_policies.clear();
    load_document_policies(json);
    QScopedValueRollback<bool> strict(_allow_connection_clock_adaptation,false);
    DataFlowGraphModel::load(json);
    restore_document_nodes(decoded);
}
void NumericEditorGraphModel::load_document_policies(const QJsonObject& json) {
    const auto saved_connections=json.value("connections").toArray();
    for(const auto value:saved_connections){auto object=value.toObject();_edge_policies[edge_key(QtNodes::fromJson(object))]=std::uint8_t(object.value("conversion").toInt());}
}
void NumericEditorGraphModel::restore_document_nodes(const domain::NumericProcessingGraph& graph) {
    for(const auto& resolved:graph.nodes)if(auto* model=delegateModel<AudioNodeModel>(QtNodes::NodeId(resolved.id));model && model->numeric_node()){
        auto node=*model->numeric_node();node.clock_node=resolved.clock_node;
        const auto binding=std::find_if(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),[&](const auto& value){return value.to==resolved.id && value.input==domain::ConfigurationRole::SampleClock;});
        if(binding!=graph.configuration_bindings.end())node.clock_node=binding->from;
        model->set_numeric_node(node);
    }
}
QJsonObject NumericEditorGraphModel::save() const {
    auto result=DataFlowGraphModel::save();
    // QtNodes stores connectivity in an unordered set. Reconnecting changed
    // contracts must not change the document solely through iteration order.
    std::vector<QJsonObject> nodes;
    const auto saved_nodes=result.value("nodes").toArray();
    for(const auto value:saved_nodes)nodes.push_back(value.toObject());
    std::sort(nodes.begin(),nodes.end(),[](const auto& a,const auto& b){return a.value("id").toInteger()<b.value("id").toInteger();});
    QJsonArray nodeArray;for(const auto& node:nodes)nodeArray.append(node);result["nodes"]=nodeArray;
    save_document_connections(result);
    save_document_metadata(result);
    return result;
}
void NumericEditorGraphModel::save_document_connections(QJsonObject& result) const {
    std::vector<QJsonObject> connections;
    const auto saved_connections=result.value("connections").toArray();
    for(const auto value:saved_connections){
        auto edge=value.toObject();const auto id=QtNodes::fromJson(edge);if(auto binding=configuration_binding(id))edge["configuration"]=int(binding->input);else edge["conversion"]=int(connection_policy(id));connections.push_back(edge);
    }
    const auto key=[](const QJsonObject& value){const auto e=QtNodes::fromJson(value);return std::tuple{e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex};};
    std::sort(connections.begin(),connections.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    QJsonArray edges;for(const auto& edge:connections)edges.append(edge);result["connections"]=edges;
}
void NumericEditorGraphModel::save_document_metadata(QJsonObject& result) const {
    if(has_numeric_nodes()){result["version"]=5;result["processing_version"]=4;result["graph_id"]=QString::fromStdString(_graph_id);result["execution_interval"]=QJsonObject{
    {"numerator",QJsonObject{{"dtype","int64"},{"decimal",QString::number(_execution_interval.numerator)}}},
    {"denominator",QJsonObject{{"dtype","uint64"},{"decimal",QString::number(_execution_interval.denominator)}}}};}
}
domain::Edge NumericEditorGraphModel::canonical_edge(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    return {domain::NodeId(edge.outNodeId),domain::NodeId(edge.inNodeId),a->port_id(QtNodes::PortType::Out,edge.outPortIndex),b->port_id(QtNodes::PortType::In,edge.inPortIndex),connection_policy(edge)};
}
QtNodes::ConnectionId NumericEditorGraphModel::connection_id(const domain::Edge& edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* a=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(edge.from));auto* b=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(edge.to));
    return {QtNodes::NodeId(edge.from),a?a->port_index(QtNodes::PortType::Out,edge.output_port):QtNodes::InvalidPortIndex,QtNodes::NodeId(edge.to),b?b->port_index(QtNodes::PortType::In,edge.input_port):QtNodes::InvalidPortIndex};
}
domain::NumericProcessingGraph NumericEditorGraphModel::numeric_graph() const {
    domain::NumericProcessingGraph graph;graph.graph_id=_graph_id;graph.versions=_versions;graph.execution_interval=_execution_interval;auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto ids=allNodeIds();std::vector<QtNodes::NodeId> sorted(ids.begin(),ids.end());std::sort(sorted.begin(),sorted.end());
    for(auto id:sorted)if(auto* node=self->delegateModel<AudioNodeModel>(id);node && node->numeric_node())graph.nodes.push_back(*node->numeric_node());
    append_numeric_graph_edges(graph,sorted);
    return graph;
}
void NumericEditorGraphModel::append_numeric_graph_edges(domain::NumericProcessingGraph& graph,
    const std::vector<QtNodes::NodeId>& sorted) const {
    for(auto id:sorted)for(auto edge:allConnectionIds(id))if(edge.outNodeId==id){
        if(auto binding=configuration_binding(edge))graph.configuration_bindings.push_back(*binding);
        else graph.edges.push_back(canonical_edge(edge));
    }
}
bool NumericEditorGraphModel::synchronize_native_sources(QString* error) {
    auto graph=numeric_graph();
    if(!resolve_native_sources(graph,error))return false;
    replace_clock_graph(graph);return true;
}
namespace {
QJsonObject selected_capture_device(const QJsonArray& choices,const QString& wanted) {
    for(const auto& value:choices) {
        const auto device=value.toObject();
        if(wanted.isEmpty()?device.value("is_default").toBool():device.value("id").toString()==wanted)return device;
    }
    return wanted.isEmpty() && !choices.isEmpty()?choices.first().toObject():QJsonObject{};
}
QString native_source_failure(const desktop::NumericSourceFormatDiagnostic& diagnostic) {
    using E=desktop::NumericSourceFormatError;
    switch(diagnostic.error) {
    case E::None:return {};
    case E::UnsupportedBounds:return ui::text("Node %1: unsupported native source format").arg(diagnostic.node);
    case E::AdmissionLimit:return ui::text("Node %1: native source format exceeds the execution profile").arg(diagnostic.node);
    case E::MissingSource:return ui::text("Node %1: source is missing").arg(diagnostic.node);
    case E::InvalidContract:return ui::text("Node %1: invalid native source contract").arg(diagnostic.node);
    case E::ConnectedOutputUnavailable:return ui::text("Node %1: connected output %2 is unavailable in the native format").arg(diagnostic.node).arg(diagnostic.port.value_or(0));
    case E::IncompatibleRoute:return ui::text("Node %1: native source format is incompatible with the route").arg(diagnostic.node);
    }
    return ui::text("Unsupported node settings");
}
bool apply_native_device_format(domain::NumericProcessingGraph& graph,domain::NodeId id,
    const QJsonObject& selected,const domain::ProcessingAdmissionLimits* limits,QString* error) {
    const auto count=selected.value("native_channels").toInt();
    const auto fs=selected.value("native_sample_rate").toInt();
    if(count<=0 || fs<=0)return true; // Unavailable devices retain their known zero-continuity contract.
    if(!limits){if(error)*error=ui::text("Native source execution profile is unavailable");return false;}
    desktop::NumericSourceFormatDiagnostic diagnostic;
    if(desktop::resolve_numeric_source_format(graph,id,std::uint32_t(fs),std::uint32_t(count),*limits,nullptr,&diagnostic))return true;
    if(error)*error=native_source_failure(diagnostic);
    return false;
}
}
bool NumericEditorGraphModel::resolve_native_source(domain::NumericProcessingGraph& graph,domain::NodeId id,const QJsonObject& adapter,QString* error) const {
    const auto source=std::find_if(graph.nodes.begin(),graph.nodes.end(),[id](const auto& node){return node.id==id;});
    if(source==graph.nodes.end())return false;
    QString discovery_error;
    const auto choices=_capture_device_provider(numeric_editor::capture_source_kind(*source),&discovery_error);
    const auto selected=selected_capture_device(choices,adapter.value("device_identity").toString());
    return apply_native_device_format(graph,id,selected,_admission_limits?&*_admission_limits:nullptr,error);
}
bool NumericEditorGraphModel::resolve_native_sources(domain::NumericProcessingGraph& graph,QString* error,const QJsonObject* imported_adapters) const {
    if(!_capture_device_provider)return true;
    std::vector<domain::NodeId> ids;
    for(const auto& node:graph.nodes)if(node.kind=="AudioSource")ids.push_back(node.id);
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    for(const auto id:ids) {
        auto* model=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(id));
        const auto adapter=imported_adapters?imported_adapters->value(QString::number(id)).toObject():model?model->adapter():QJsonObject{};
        if(!resolve_native_source(graph,id,adapter,error))return false;
    }
    return true;
}

namespace {
bool resolve_export_configuration(domain::NumericProcessingGraph& graph,QString* error) {
    domain::NumericProcessingGraph resolved;
    if(!domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved)){
        if(error)*error=ui::text("Invalid configuration connection");return false;
    }
    graph=std::move(resolved);return true;
}
bool resolve_export_parameters(domain::NumericProcessingGraph& graph,QString* error) {
    domain::NumericProcessingGraph resolved,executable;
    if(!domain::resolve_numeric_parameter_bindings(graph,numeric_editor::limits(),resolved,executable)){
        if(error)*error=ui::text("Unsupported node settings");return false;
    }
    graph=std::move(resolved);return true;
}
bool export_clock_valid(const domain::ConfiguredProcessingNode& node,const domain::NumericProcessingGraph& graph,QString* error) {
    if(node.kind=="SignalGenerator" && node.clock_node<0)return true;
    if(node.kind!="SignalGenerator" && (node.kind!="Mixer" || std::get<domain::MixerProcessingParameters>(node.parameters).consumption!=domain::MixerConsumption::ClockedFifo))return true;
    const auto clock=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==node.clock_node && (n.kind=="SampleClock" || (n.kind=="SignalGenerator" && n.clock_node<0));});
    if(clock!=graph.nodes.end())return true;
    if(error)*error=ui::text("Node %1 (%2): select a synchronization source in block settings").arg(node.id).arg(QString::fromStdString(node.kind));
    return false;
}
bool import_adapter_key_valid(const domain::NumericProcessingGraph& graph,const QString& key) {
    bool valid=false;const auto id=key.toLongLong(&valid);
    return valid && id>=0 && QString::number(id)==key && std::any_of(graph.nodes.begin(),graph.nodes.end(),[id](const auto& node){return node.id==id;});
}
bool decode_import_adapters(const domain::NumericProcessingGraph& graph,const QJsonObject& input,QJsonObject& out,QString* error) {
    for(auto it=input.begin();it!=input.end();++it){
        if(!import_adapter_key_valid(graph,it.key()) || !it.value().isObject()){
            if(error)*error=ui::text("Invalid network source settings");return false;
        }
        QJsonObject adapter;
        const auto result=desktop::decode_numeric_node_adapter(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact),adapter);
        if(!result){if(error)*error=result.error;return false;}
        out[it.key()]=adapter;
    }
    return true;
}
}
bool NumericEditorGraphModel::validate_export_nodes(const domain::NumericProcessingGraph& graph,QString* error) const {
    if(!domain::valid_execution_interval(graph.execution_interval)){if(error)*error=ui::text("Invalid execution interval");return false;}
    for(const auto& node:graph.nodes)if(!export_clock_valid(node,graph,error))return false;
    return validate_export_node_diagnostics(graph,error);
}
bool NumericEditorGraphModel::validate_export_node_diagnostics(const domain::NumericProcessingGraph& graph,QString* error) const {
    for(const auto& node:graph.nodes){
        const auto diagnostic=node_error(node);
        if(!diagnostic.isEmpty()){if(error)*error=QString::fromStdString(node.kind)+": "+diagnostic;return false;}
    }
    return true;
}
bool NumericEditorGraphModel::export_numeric_graph(domain::NumericProcessingGraph& out,QString* error,bool resolve_native) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    for(auto id:allNodeIds())if(auto* node=self->delegateModel<AudioNodeModel>(id);!node || !node->numeric_node()){
        if(error)*error=ui::text("Unsupported numeric draft version");return false;
    }
    auto graph=numeric_graph();
    if(resolve_native && (!resolve_export_configuration(graph,error) || !resolve_native_sources(graph,error) || !resolve_export_parameters(graph,error) || !resolve_export_configuration(graph,error) || !validate_export_nodes(graph,error)))return false;
    if(!domain::valid_execution_interval(graph.execution_interval))return false;
    out=std::move(graph);return true;
}
bool NumericEditorGraphModel::prepare_numeric_import(const domain::NumericProcessingGraph& requested,
    const QJsonObject& node_adapters,domain::NumericProcessingGraph& out,QString* error) const {
    QJsonObject checked;
    if(!decode_import_adapters(requested,node_adapters,checked,error))return false;
    auto graph=requested;
    for(auto& node:graph.nodes)if(domain::attach_numeric_parameter_ports(node,numeric_editor::limits())!=signal_contracts::Status::ok)return false;
    // The imported document owns adapter selection. Even an empty map means
    // OS default, never a prior document's node with the same numeric ID.
    if(!resolve_native_sources(graph,error,&checked))return false;
    out=std::move(graph);return true;
}
namespace {
bool draft_failure(QString* error,const char* key){if(error)*error=ui::text(key);return false;}
bool draft_header_valid(const domain::NumericProcessingGraph& graph,QString* error){
    if(domain::processing_versions_supported(graph.versions)!=signal_contracts::Status::ok)return draft_failure(error,"Unsupported numeric draft version");
    if(!domain::valid_execution_interval(graph.execution_interval))return draft_failure(error,"Invalid execution interval");
    if(graph.nodes.size()>numeric_editor::limits().max_nodes || graph.edges.size()>numeric_editor::limits().max_edges)return draft_failure(error,"Draft exceeds editor limits");
    return true;
}
}
bool NumericEditorGraphModel::load_draft_node(const domain::ConfiguredProcessingNode& node,const QJsonObject& adapters,QString* error){
    if(node.id<0 || std::uint64_t(node.id)>=std::numeric_limits<QtNodes::NodeId>::max() || nodeExists(QtNodes::NodeId(node.id)) || !dataModelRegistry()->create(QString::fromStdString(node.kind)))return draft_failure(error,"Draft contains unsupported nodes");
    const QJsonObject json{{"id",node.id},{"internal-data",QJsonObject{{"model-name",QString::fromStdString(node.kind)}}}};
    loadNode(json);auto* model=delegateModel<AudioNodeModel>(QtNodes::NodeId(node.id));
    if(!model)return draft_failure(error,"Draft contains unsupported nodes");
    model->set_numeric_node(node);
    if(!model->set_adapter(adapters.value(QString::number(node.id)).toObject()))return draft_failure(error,"Invalid network source settings");
    return true;
}
bool NumericEditorGraphModel::load_draft_edges(const domain::NumericProcessingGraph& graph,QString* error){
    reveal_connected_dc(graph);
    for(const auto& edge:graph.edges){
        if(edge.conversion>3)return draft_failure(error,"Invalid connection policy");
        const auto id=connection_id(edge);
        if(edge.from<0 || edge.to<0 || !connectionPossible(id)){
            if(error)*error=ui::text("Draft connection has invalid endpoints: %1, output %2 → %3, input %4").arg(edge.from).arg(edge.output_port).arg(edge.to).arg(edge.input_port);
            return false;
        }
        addConnection(id);
    }
    return true;
}
bool NumericEditorGraphModel::load_draft_bindings(const domain::NumericProcessingGraph& graph,QString* error){
    for(const auto& binding:graph.configuration_bindings){
        const auto edge=configuration_connection(binding);
        if(!connectionPossible(edge))return draft_failure(error,"Draft contains a missing endpoint");
        addConnection(edge);
    }
    return true;
}
void NumericEditorGraphModel::install_draft_graph(const domain::NumericProcessingGraph& graph,const NumericEditorGraphModel& candidate){
    QScopedValueRollback<bool> structural(_allow_connection_clock_adaptation,false);
    for(auto id:allNodeIds())deleteNode(id);
    _graph_id=graph.graph_id;_versions=graph.versions;_execution_interval=graph.execution_interval;
    install_draft_nodes(graph,candidate);
    install_draft_edges(graph);
}
void NumericEditorGraphModel::install_draft_nodes(const domain::NumericProcessingGraph& graph,const NumericEditorGraphModel& candidate) {
    for(const auto& node:graph.nodes){
        loadNode(candidate.saveNode(QtNodes::NodeId(node.id)));
        if(auto* model=delegateModel<AudioNodeModel>(QtNodes::NodeId(node.id));model && model->numeric_node()){
            auto restored=*model->numeric_node();restored.clock_node=node.clock_node;model->set_numeric_node(restored);
        }
    }
}
void NumericEditorGraphModel::install_draft_edges(const domain::NumericProcessingGraph& graph) {
    reveal_connected_dc(graph);_edge_policies.clear();
    for(const auto& edge:graph.edges){const auto id=connection_id(edge);_edge_policies[edge_key(id)]=edge.conversion;addConnection(id);}
    for(const auto& binding:graph.configuration_bindings)DataFlowGraphModel::addConnection(configuration_connection(binding));
}
bool NumericEditorGraphModel::set_numeric_graph(const domain::NumericProcessingGraph& requested,QString* error,const QJsonObject& node_adapters){
    domain::NumericProcessingGraph graph;
    if(!prepare_numeric_import(requested,node_adapters,graph,error) || !draft_header_valid(graph,error))return false;
    NumericEditorGraphModel candidate(dataModelRegistry());candidate.set_node_validator(_node_validator);candidate.set_admission_limits(_admission_limits);candidate._allow_connection_clock_adaptation=false;
    for(const auto& node:graph.nodes)if(!candidate.load_draft_node(node,node_adapters,error))return false;
    if(!candidate.load_draft_edges(graph,error) || !candidate.load_draft_bindings(graph,error))return false;
    install_draft_graph(graph,candidate);return true;
}
