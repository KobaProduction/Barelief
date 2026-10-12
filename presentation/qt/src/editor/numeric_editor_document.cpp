#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "visualization/ui_text.h"
#include <desktop/numeric_project_document.h>
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
}
void NumericEditorGraphModel::load(const QJsonObject& json){
    if(json.value("version").toInt()!=5 || json.value("processing_version").toInt()!=4)return;
    domain::NumericProcessingGraph decoded;
    if(!desktop::decode_numeric_editor_graph(QJsonDocument(json).toJson(QJsonDocument::Compact),decoded))return;
    domain::NumericProcessingGraph configured;
    if(!domain::resolve_configuration_bindings(decoded,numeric_editor::limits(),configured))return;
    _execution_interval=decoded.execution_interval;
    _graph_id=json.value("graph_id").toString("main").toStdString();_edge_policies.clear();
    for(const auto value:json.value("connections").toArray()){auto object=value.toObject();_edge_policies[edge_key(QtNodes::fromJson(object))]=std::uint8_t(object.value("conversion").toInt());}
    QScopedValueRollback<bool> strict(_allow_connection_clock_adaptation,false);
    DataFlowGraphModel::load(json);
    for(const auto& resolved:configured.nodes)if(auto* model=delegateModel<AudioNodeModel>(QtNodes::NodeId(resolved.id));model && model->numeric_node()){
        auto node=*model->numeric_node();node.clock_node=resolved.clock_node;model->set_numeric_node(node);
    }
}
QJsonObject NumericEditorGraphModel::save() const {
    auto result=DataFlowGraphModel::save();
    // QtNodes stores connectivity in an unordered set. Reconnecting changed
    // contracts must not change the document solely through iteration order.
    std::vector<QJsonObject> nodes,connections;
    for(const auto value:result.value("nodes").toArray())nodes.push_back(value.toObject());
    std::sort(nodes.begin(),nodes.end(),[](const auto& a,const auto& b){return a.value("id").toInteger()<b.value("id").toInteger();});
    QJsonArray nodeArray;for(const auto& node:nodes)nodeArray.append(node);result["nodes"]=nodeArray;
    for(const auto value:result.value("connections").toArray()){
        auto edge=value.toObject();const auto id=QtNodes::fromJson(edge);if(auto binding=configuration_binding(id))edge["configuration"]=int(binding->input);else edge["conversion"]=int(connection_policy(id));connections.push_back(edge);
    }
    const auto key=[](const QJsonObject& value){const auto e=QtNodes::fromJson(value);return std::tuple{e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex};};
    std::sort(connections.begin(),connections.end(),[&](const auto& a,const auto& b){return key(a)<key(b);});
    QJsonArray edges;for(const auto& edge:connections)edges.append(edge);result["connections"]=edges;
    if(has_numeric_nodes()){result["version"]=5;result["processing_version"]=4;result["graph_id"]=QString::fromStdString(_graph_id);result["execution_interval"]=QJsonObject{
    {"numerator",QJsonObject{{"dtype","int64"},{"decimal",QString::number(_execution_interval.numerator)}}},
    {"denominator",QJsonObject{{"dtype","uint64"},{"decimal",QString::number(_execution_interval.denominator)}}}};}return result;}
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
    for(auto id:sorted)for(auto edge:allConnectionIds(id))if(edge.outNodeId==id){if(auto binding=configuration_binding(edge))graph.configuration_bindings.push_back(*binding);else graph.edges.push_back(canonical_edge(edge));}
    return graph;
}
bool NumericEditorGraphModel::synchronize_native_sources(QString* error) {
    auto graph=numeric_graph();
    if(!resolve_native_sources(graph,error))return false;
    replace_clock_graph(graph);return true;
}
bool NumericEditorGraphModel::resolve_native_sources(domain::NumericProcessingGraph& graph,QString* error,const QJsonObject* imported_adapters) const {
    if(!_capture_device_provider)return true;
    auto* self=const_cast<NumericEditorGraphModel*>(this);std::vector<domain::NodeId> ids;
    for(const auto& n:graph.nodes)if(n.kind=="AudioSource")ids.push_back(n.id);
    for(const auto id:ids) {
        const domain::ConfiguredProcessingNode* source=nullptr;for(const auto& n:graph.nodes)if(n.id==id)source=&n;if(!source)return false;
        auto* model=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(id));
        const auto adapter=imported_adapters?imported_adapters->value(QString::number(id)).toObject():model?model->adapter():QJsonObject{};
        const auto wanted=adapter.value("device_identity").toString();
        QString diagnostic;const auto choices=_capture_device_provider(numeric_editor::capture_source_kind(*source),&diagnostic);
        QJsonObject selected;for(const auto& v:choices){const auto c=v.toObject();if(wanted.isEmpty()?c.value("is_default").toBool():c.value("id").toString()==wanted){selected=c;break;}}
        if(selected.isEmpty() && wanted.isEmpty() && !choices.isEmpty())selected=choices.first().toObject();
        const auto count=selected.value("native_channels").toInt();const auto fs=selected.value("native_sample_rate").toInt();
        if(!count || !fs)continue; // Unavailable device retains its known contract for zero continuity.
        if(!desktop::resolve_numeric_source_format(graph,id,std::uint32_t(fs),std::uint32_t(count),error))return false;
    }return true;
}

bool NumericEditorGraphModel::export_numeric_graph(domain::NumericProcessingGraph& out,QString* error) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    for(auto id:allNodeIds())if(auto* node=self->delegateModel<AudioNodeModel>(id);!node || !node->numeric_node()){
        if(error)*error=ui::text("Unsupported numeric draft version");return false;
    }
    out=numeric_graph();
    domain::NumericProcessingGraph resolved;if(!domain::resolve_configuration_bindings(out,numeric_editor::limits(),resolved)){if(error)*error=ui::text("Invalid configuration connection");return false;}out=std::move(resolved);
    if(!resolve_native_sources(out,error))return false;
    if(!domain::resolve_configuration_bindings(out,numeric_editor::limits(),resolved)){if(error)*error=ui::text("Invalid configuration connection");return false;}out=std::move(resolved);
    if(!domain::valid_execution_interval(out.execution_interval)){if(error)*error=ui::text("Invalid execution interval");return false;}
    for(const auto& node:out.nodes)if(node.kind=="SignalGenerator" || (node.kind=="Mixer" && std::get<domain::MixerProcessingParameters>(node.parameters).consumption==domain::MixerConsumption::ClockedFifo)){
        const auto clock=std::find_if(out.nodes.begin(),out.nodes.end(),[&](const auto& n){return n.id==node.clock_node && n.kind=="SampleClock";});
        if(clock==out.nodes.end()){if(error)*error=ui::text("Node %1 (%2): select an explicit SampleClock in block settings").arg(node.id).arg(QString::fromStdString(node.kind));return false;}
    }
    for(const auto& node:out.nodes){const auto diagnostic=node_error(node);if(!diagnostic.isEmpty()){if(error)*error=QString::fromStdString(node.kind)+": "+diagnostic;return false;}}return true;
}
bool NumericEditorGraphModel::prepare_numeric_import(const domain::NumericProcessingGraph& requested,
    const QJsonObject& node_adapters,domain::NumericProcessingGraph& out,QString* error) const {
    QJsonObject checked;
    for(auto it=node_adapters.begin();it!=node_adapters.end();++it) {
        bool valid=false;const auto id=it.key().toLongLong(&valid);QJsonObject adapter;
        if(!valid || id<0 || QString::number(id)!=it.key() ||
            std::none_of(requested.nodes.begin(),requested.nodes.end(),[&](const auto& n){return n.id==id;}) || !it.value().isObject()) {
            if(error)*error=ui::text("Invalid network source settings");return false;
        }
        const auto result=desktop::decode_numeric_node_adapter(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact),adapter);
        if(!result){if(error)*error=result.error;return false;}checked[it.key()]=adapter;
    }
    auto graph=requested;
    if(!resolve_native_sources(graph,error,&checked))return false;
    domain::NumericProcessingGraph resolved;if(!domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved)){if(error)*error=ui::text("Invalid configuration connection");return false;}out=std::move(resolved);return true;
}
bool NumericEditorGraphModel::set_numeric_graph(const domain::NumericProcessingGraph& requested,QString* error,const QJsonObject& node_adapters) {
    // Import the physical endpoint's actual contract before publishing the draft.
    // Otherwise export would silently resolve a different graph on a copy and
    // disable previews because the displayed draft could never match its Apply.
    domain::NumericProcessingGraph graph;
    if(!prepare_numeric_import(requested,node_adapters,graph,error))return false;
    // Preflight without a scene: unsupported nodes or edges must not partially
    // replace the current draft or disappear on the next source-backed save.
    NumericEditorGraphModel candidate(dataModelRegistry());candidate.set_node_validator(_node_validator);candidate._allow_connection_clock_adaptation=false;
    if(domain::processing_versions_supported(graph.versions)!=signal_contracts::Status::ok){if(error)*error=ui::text("Unsupported numeric draft version");return false;}
    if(!domain::valid_execution_interval(graph.execution_interval)){if(error)*error=ui::text("Invalid execution interval");return false;}
    if(graph.nodes.size()>numeric_editor::limits().max_nodes || graph.edges.size()>numeric_editor::limits().max_edges){if(error)*error=ui::text("Draft exceeds editor limits");return false;}
    for(const auto& node:graph.nodes){
        if(node.id<0 || std::uint64_t(node.id)>=std::numeric_limits<QtNodes::NodeId>::max() || candidate.nodeExists(QtNodes::NodeId(node.id)) || !dataModelRegistry()->create(QString::fromStdString(node.kind))){
            if(error)*error=ui::text("Draft contains unsupported nodes");return false;
        }
        QJsonObject json{{"id",node.id},{"internal-data",QJsonObject{{"model-name",QString::fromStdString(node.kind)}}}};
        candidate.loadNode(json);auto* model=candidate.delegateModel<AudioNodeModel>(QtNodes::NodeId(node.id));
        if(!model){if(error)*error=ui::text("Draft contains unsupported nodes");return false;}model->set_numeric_node(node);
        if(!model->set_adapter(node_adapters.value(QString::number(node.id)).toObject())){if(error)*error=ui::text("Invalid network source settings");return false;}
    }
    for(const auto& edge:graph.edges){
        if(edge.conversion>3){if(error)*error=ui::text("Invalid connection policy");return false;}
        const auto id=candidate.connection_id(edge);
        if(edge.from<0 || edge.to<0 || !candidate.connectionPossible(id)){
            if(error){
                const auto label=[&](domain::NodeId node){
                    const auto found=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==node;});
                    return found==graph.nodes.end()?ui::text("Unavailable"):AudioNodeModel(QString::fromStdString(found->kind)).caption();
                };
                *error=ui::text("Draft contains incompatible connection: %1 [%2], output %3 → %4 [%5], input %6")
                    .arg(label(edge.from)).arg(edge.from).arg(edge.output_port)
                    .arg(label(edge.to)).arg(edge.to).arg(edge.input_port);
            }return false;
        }
        candidate.addConnection(id);
    }
    for(auto id:allNodeIds())deleteNode(id);_graph_id=graph.graph_id;_versions=graph.versions;_execution_interval=graph.execution_interval;
    for(const auto& node:graph.nodes){loadNode(candidate.saveNode(QtNodes::NodeId(node.id)));}
    _edge_policies.clear();for(const auto& edge:graph.edges){const auto id=connection_id(edge);_edge_policies[edge_key(id)]=edge.conversion;addConnection(id);}
    for(const auto& binding:graph.configuration_bindings)DataFlowGraphModel::addConnection(configuration_connection(binding));
    return true;
}
