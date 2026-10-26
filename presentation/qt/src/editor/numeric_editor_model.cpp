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
#include <span>
#include <tuple>
#include <type_traits>
#include "numeric_graph_draft.h"
#include <domain/graph/node_factory.h>
#include <QScopedValueRollback>
namespace {
std::optional<signal_contracts::ClockContract> synchronization_clock(const domain::ConfiguredProcessingNode& node) {
    if(const auto* clock=std::get_if<domain::SampleClockProcessingParameters>(&node.parameters))return signal_contracts::ClockContract{clock->domain,{1,clock->sample_rate},clock->sample_rate};
    if(node.kind=="SignalGenerator" && node.clock_node<0 && !node.outputs.empty()){
        const auto data=node.outputs.front().contract.view();if(const auto* numeric=std::get_if<signal_contracts::NumericContract>(&data))return numeric->clock;
    }return {};
}
}

NumericEditorGraphModel::NumericEditorGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry):DataFlowGraphModel(std::move(registry)) {
    const auto invalidate=[this]{_connection_visual_states.clear();};
    connect(this,&NumericEditorGraphModel::nodeUpdated,this,invalidate);
    connect(this,&NumericEditorGraphModel::nodeCreated,this,invalidate);
    connect(this,&NumericEditorGraphModel::nodeDeleted,this,invalidate);
    connect(this,&NumericEditorGraphModel::connectionCreated,this,invalidate);
    connect(this,&NumericEditorGraphModel::connectionDeleted,this,invalidate);
    connect(this,&NumericEditorGraphModel::connection_diagnostics_changed,this,invalidate);
}
bool NumericEditorGraphModel::has_numeric_nodes() const {return true;}
void NumericEditorGraphModel::set_admission_limits(std::optional<domain::ProcessingAdmissionLimits> limits) {
    _admission_limits=std::move(limits);
    for(const auto id:allNodeIds())if(auto* node=delegateModel<AudioNodeModel>(id))node->set_admission_limits(_admission_limits);
}
QString NumericEditorGraphModel::node_error(const domain::ConfiguredProcessingNode& node) const {
    domain::ResolvedProcessingNode resolved;
    const auto diagnostic=domain::resolve_processing_node(node,numeric_editor::limits(),resolved);
    if(!diagnostic)return numeric_editor::processing_diagnostic_text(diagnostic);
    if(_node_validator){const auto support=_node_validator(node);if(!support)return numeric_editor::processing_diagnostic_text(support);}
    return {};
}
std::optional<domain::ConfigurationBinding> NumericEditorGraphModel::configuration_binding(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b)return {};
    const auto out=a->configuration_role(QtNodes::PortType::Out,edge.outPortIndex),in=b->configuration_role(QtNodes::PortType::In,edge.inPortIndex);
    if(!out || !in)return {};return domain::ConfigurationBinding{domain::NodeId(edge.outNodeId),domain::NodeId(edge.inNodeId),*out,*in};
}
QtNodes::ConnectionId NumericEditorGraphModel::configuration_connection(const domain::ConfigurationBinding& b) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(b.from));auto* z=self->delegateModel<AudioNodeModel>(QtNodes::NodeId(b.to));
    return {QtNodes::NodeId(b.from),a?a->configuration_port(QtNodes::PortType::Out,b.output):QtNodes::InvalidPortIndex,QtNodes::NodeId(b.to),z?z->configuration_port(QtNodes::PortType::In,b.input):QtNodes::InvalidPortIndex};
}
bool NumericEditorGraphModel::bind_configuration_clock(const domain::ConfiguredProcessingNode& from,
    domain::ConfiguredProcessingNode& to,const domain::ConfigurationBinding& binding,
    domain::NumericProcessingGraph& graph) const {
    const auto clock=synchronization_clock(from);if(!clock)return false;
    if(!numeric_editor::bind_target_clock(to,*clock))return false;
    to.clock_node=from.id;graph.configuration_bindings.push_back(binding);
    return propagate_clock(graph,to.id);
}
bool NumericEditorGraphModel::adapt_configuration(QtNodes::ConnectionId edge,domain::NumericProcessingGraph& graph) const {
    const auto binding=configuration_binding(edge);if(!binding || binding->output!=binding->input)return false;
    auto from=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==binding->from;});auto to=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==binding->to;});
    if(from==graph.nodes.end() || to==graph.nodes.end())return false;
    if(binding->input==domain::ConfigurationRole::SampleClock && (to->kind=="SignalGenerator" || to->kind=="Mixer" || to->kind=="PcmResampler")){
        if(!bind_configuration_clock(*from,*to,*binding,graph))return false;
    } else graph.configuration_bindings.push_back(*binding);
    domain::NumericProcessingGraph resolved;
    return bool(domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved));
}
bool NumericEditorGraphModel::connection_endpoints_available(QtNodes::ConnectionId edge) const {
    if(!nodeExists(edge.outNodeId) || !nodeExists(edge.inNodeId) || edge.outNodeId==edge.inNodeId)return false;
    if(edge.outPortIndex>=nodeData(edge.outNodeId,QtNodes::NodeRole::OutPortCount).toUInt() ||
       edge.inPortIndex>=nodeData(edge.inNodeId,QtNodes::NodeRole::InPortCount).toUInt())return false;
    for(const auto existing:allConnectionIds(edge.inNodeId))
        if(existing.inNodeId==edge.inNodeId && existing.inPortIndex==edge.inPortIndex)return false;
    return true;
}
bool NumericEditorGraphModel::connection_would_cycle(QtNodes::ConnectionId edge) const {
    std::vector<QtNodes::NodeId> pending{edge.inNodeId};std::set<QtNodes::NodeId> visited;
    for(std::size_t cursor=0;cursor<pending.size();++cursor){
        const auto node=pending[cursor];if(node==edge.outNodeId)return true;
        if(!visited.insert(node).second)continue;
        for(const auto existing:allConnectionIds(node))if(existing.outNodeId==node)pending.push_back(existing.inNodeId);
    }
    return false;
}
bool NumericEditorGraphModel::connectionPossible(QtNodes::ConnectionId edge) const {
    return connection_endpoints_available(edge) && !connection_would_cycle(edge);
}

namespace {
auto edge_key(QtNodes::ConnectionId e){return std::make_tuple(e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex);}
class GraphParameterEdit final:public QUndoCommand {
    std::function<void(bool)> apply_;
public:
    GraphParameterEdit(QString title,std::function<void(bool)> apply):apply_(std::move(apply)){setText(title);}
    void redo()override{apply_(true);}void undo()override{apply_(false);}
};
bool same_clock_ports(const domain::ConfiguredProcessingNode& a,const domain::ConfiguredProcessingNode& b){
    for(auto pair:{std::pair{&a.inputs,&b.inputs},std::pair{&a.outputs,&b.outputs}}){
        if(pair.first->size()!=pair.second->size())return false;
        for(std::size_t i=0;i<pair.first->size();++i)if((*pair.first)[i].id!=(*pair.second)[i].id || (*pair.first)[i].direction!=(*pair.second)[i].direction || !signal_contracts::same_contract((*pair.first)[i].contract.view(),(*pair.second)[i].contract.view()))return false;
    }
    return true;
}
bool interactive_selection(const domain::ConfiguredProcessingNode& before,const domain::ConfiguredProcessingNode& after){
    return (after.kind=="WindowFunction" || after.kind=="MatrixCenter" || after.kind=="MatrixWindowFunction" || after.kind=="MatrixRow") &&
        before.clock_node==after.clock_node && same_clock_ports(before,after);
}
bool append_changed_node_state(const domain::NumericProcessingGraph& original,
    const domain::NumericProcessingGraph& graph,QJsonArray& before,QJsonArray& after,
    QJsonArray& before_clocks,QJsonArray& after_clocks){
    for(std::size_t i=0;i<graph.nodes.size();++i){
        const auto& old_node=original.nodes[i];const auto& new_node=graph.nodes[i];
        if(same_clock_ports(old_node,new_node) && old_node.clock_node==new_node.clock_node &&
           domain::same_processing_parameters(old_node.parameters,new_node.parameters))continue;
        QByteArray old_bytes,new_bytes;
        if(!desktop::encode_numeric_node(old_node,old_bytes) || !desktop::encode_numeric_node(new_node,new_bytes))return false;
        before.append(QString::fromUtf8(old_bytes));after.append(QString::fromUtf8(new_bytes));
        before_clocks.append(QString::number(old_node.clock_node));after_clocks.append(QString::number(new_node.clock_node));
    }
    return true;
}
enum class UndoRestore { Applied, DeleteConnection, InvalidState };
UndoRestore decode_undo_node(const QJsonArray& before,const QJsonArray& after,
    const QJsonArray& before_clocks,const QJsonArray& after_clocks,int index,bool binding,
    domain::ConfiguredProcessingNode& old_node,domain::ConfiguredProcessingNode& new_node){
    if(!desktop::decode_numeric_node(before[index].toString().toUtf8(),old_node) ||
       !desktop::decode_numeric_node(after[index].toString().toUtf8(),new_node) ||
       old_node.id!=new_node.id)return UndoRestore::DeleteConnection;
    if(!binding)return UndoRestore::Applied;
    if(before_clocks.size()!=before.size() || after_clocks.size()!=after.size())return UndoRestore::InvalidState;
    bool old_ok=false,new_ok=false;
    old_node.clock_node=before_clocks[index].toString().toLongLong(&old_ok);
    new_node.clock_node=after_clocks[index].toString().toLongLong(&new_ok);
    return old_ok && new_ok?UndoRestore::Applied:UndoRestore::InvalidState;
}
void rollback_undo_node(domain::ConfiguredProcessingNode& node,
    const domain::ConfiguredProcessingNode& before,const domain::ConfiguredProcessingNode& after,
    bool binding,bool parameter){
    // Keep later edits; roll back only contracts and settings still matching this connection.
    node.inputs=before.inputs;node.outputs=before.outputs;
    if(binding){
        if(node.clock_node==after.clock_node)node.clock_node=before.clock_node;
        if(domain::same_processing_parameters(node.parameters,after.parameters))node.parameters=before.parameters;
    }else if(parameter && domain::same_processing_parameters(node.parameters,after.parameters))node.parameters=before.parameters;
}
UndoRestore restore_undo_nodes(domain::NumericProcessingGraph& graph,const QJsonArray& before,
    const QJsonArray& after,const QJsonArray& before_clocks,const QJsonArray& after_clocks,
    bool binding,bool parameter){
    for(int index=0;index<after.size();++index){
        domain::ConfiguredProcessingNode old_node,new_node;
        const auto status=decode_undo_node(before,after,before_clocks,after_clocks,index,binding,old_node,new_node);
        if(status!=UndoRestore::Applied)return status;
        for(auto& node:graph.nodes)if(node.id==new_node.id && same_clock_ports(node,new_node))
            rollback_undo_node(node,old_node,new_node,binding,parameter);
    }
    return UndoRestore::Applied;
}
}
std::uint8_t NumericEditorGraphModel::connection_policy(QtNodes::ConnectionId edge) const {
    const auto it=_edge_policies.find(edge_key(edge));return it==_edge_policies.end()?0:it->second;
}
std::pair<AudioNodeModel*,AudioNodeModel*> NumericEditorGraphModel::connection_models(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    return {self->delegateModel<AudioNodeModel>(edge.outNodeId),self->delegateModel<AudioNodeModel>(edge.inNodeId)};
}
std::pair<const domain::ProcessingPort*,const domain::ProcessingPort*>
NumericEditorGraphModel::data_connection_ports(QtNodes::ConnectionId edge) const {
    const auto [source,target]=connection_models(edge);
    if(!source || !target || !source->numeric_node() || !target->numeric_node())return {nullptr,nullptr};
    const auto& outputs=source->numeric_node()->outputs;const auto& inputs=target->numeric_node()->inputs;
    const auto out=std::find_if(outputs.begin(),outputs.end(),[&](const auto& port){return port.id==source->port_id(QtNodes::PortType::Out,edge.outPortIndex);});
    const auto in=std::find_if(inputs.begin(),inputs.end(),[&](const auto& port){return port.id==target->port_id(QtNodes::PortType::In,edge.inPortIndex);});
    return {out==outputs.end()?nullptr:&*out,in==inputs.end()?nullptr:&*in};
}
QString NumericEditorGraphModel::connection_status(QtNodes::ConnectionId edge) const {
    if(configuration_binding(edge))return ui::text(connection_visual_state(edge)==3?"Invalid configuration connection":"Configuration dependency");
    const auto [output,input]=data_connection_ports(edge);
    if(!output || !input)return ui::text("Unavailable");
    if(domain::processing_ports_compatible(*output,*input))return ui::text("Exact connection");
    if(!domain::processing_ports_dtype_convertible(*output,*input))return ui::text("Incompatible connection");
    const auto from=std::get<signal_contracts::NumericContract>(output->contract.view()).dtype,to=std::get<signal_contracts::NumericContract>(input->contract.view()).dtype;
    return ui::text(domain::dtype_conversion_is_exact(from,to)?"Exact dtype widening":"Potential loss: range / rounding / precision")+"\n"+ui::text("Nonfinite values are rejected");
}
bool NumericEditorGraphModel::edit_connection_policy(QtNodes::ConnectionId edge,std::uint8_t value) {
    if(value>3 || configuration_binding(edge) || !connectionExists(edge) || connection_policy(edge)==value)return false;
    const auto before=connection_policy(edge);
    auto apply=[this,edge](std::uint8_t v){_edge_policies[edge_key(edge)]=v;emit persistence_topology_changed();emit nodeUpdated(edge.inNodeId);};
    class Edit final:public QUndoCommand{std::function<void(bool)> action;public:Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Connection conversion"));}void redo()override{action(true);}void undo()override{action(false);}};
    if(_undo)_undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));else apply(value);
    return true;
}
QString NumericEditorGraphModel::connection_failure(QtNodes::ConnectionId edge) const {
    if(const auto binding=configuration_binding(edge))return binding->output==binding->input?QString{}:ui::text("Invalid configuration connection");
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(a){const auto error=a->diagnostic_failure(domain::PortDirection::Output,a->port_id(QtNodes::PortType::Out,edge.outPortIndex));if(!error.isEmpty())return error;}
    if(b){const auto error=b->diagnostic_failure(domain::PortDirection::Input,b->port_id(QtNodes::PortType::In,edge.inPortIndex));if(!error.isEmpty())return error;}
    return connection_visual_state(edge)==3?ui::text("Incompatible connection: check dtype, shape, layout, clock and coordinates"):QString{};
}
int NumericEditorGraphModel::connection_visual_state(QtNodes::ConnectionId edge) const {
    const auto key=edge_key(edge);
    const auto cached=_connection_visual_states.find(key);
    if(cached!=_connection_visual_states.end())return cached->second;
    const auto state=compute_connection_visual_state(edge);
    // Incomplete/transient queries must not grow a cache beyond the live graph.
    if(connectionExists(edge))_connection_visual_states.emplace(key,state);
    return state;
}
int NumericEditorGraphModel::compute_connection_visual_state(QtNodes::ConnectionId edge) const {
    if(const auto binding=configuration_binding(edge))return binding->output==binding->input?0:3;
    const auto [source,target]=connection_models(edge);
    const auto [output,input]=data_connection_ports(edge);
    if(!source || !target || !output || !input)return 3;
    // Runtime failures belong to the port/node diagnostics, not to the
    // physical connection contract: an exact data link must stay visible.
    if(domain::processing_ports_compatible(*output,*input))return 0;
    if(!source->diagnostic_failure(domain::PortDirection::Output,output->id).isEmpty() ||
       !target->diagnostic_failure(domain::PortDirection::Input,input->id).isEmpty())return 3;
    if(!domain::processing_ports_dtype_convertible(*output,*input))return 3;
    return domain::dtype_conversion_is_exact(std::get<signal_contracts::NumericContract>(output->contract.view()).dtype,
        std::get<signal_contracts::NumericContract>(input->contract.view()).dtype)?1:2;
}
QString NumericEditorGraphModel::connection_effects(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node() || edge.outPortIndex>=a->numeric_node()->outputs.size() || edge.inPortIndex>=b->numeric_node()->inputs.size())return {};
    const auto out=a->numeric_node()->outputs[edge.outPortIndex].contract.view(),in=b->numeric_node()->inputs[edge.inPortIndex].contract.view();
    const auto* x=std::get_if<signal_contracts::NumericContract>(&out);const auto* y=std::get_if<signal_contracts::NumericContract>(&in);
    return x && y?numeric_editor::conversion_effects(x->dtype,y->dtype):QString{};
}
void NumericEditorGraphModel::add_adapted_connection(QtNodes::ConnectionId edge) {
    // Adaptation may update explicitly connected port geometry. A failed
    // preparation must still retain the structural draft and its diagnostic.
    auto graph=numeric_graph();
    const bool adapted=_allow_connection_clock_adaptation &&
        (configuration_binding(edge)?adapt_configuration(edge,graph):adapt_connection_clock(edge,graph));
    if(adapted)replace_clock_graph(graph);else DataFlowGraphModel::addConnection(edge);
}
void NumericEditorGraphModel::addConnection(QtNodes::ConnectionId edge) {
    if(connectionExists(edge))return;
    const auto* source=delegateModel<AudioNodeModel>(edge.outNodeId);
    const bool dc=source && (source->name()=="Fourier" || source->name()=="RealFourier") && edge.outPortIndex>=2 && edge.outPortIndex<=3;
    if(dc)delegateModel<AudioNodeModel>(edge.outNodeId)->set_dc_connections_visible(true);
    if(!connectionPossible(edge)){if(dc)refresh_connected_dc(edge.outNodeId);return;}
    add_adapted_connection(edge);
    if(dc && !connectionExists(edge))refresh_connected_dc(edge.outNodeId);
    emit connection_diagnostics_changed();
}

void NumericEditorGraphModel::clear_removed_clock_binding(QtNodes::ConnectionId edge) {
    if(auto* target=delegateModel<AudioNodeModel>(edge.inNodeId);target && target->numeric_node()){
        auto node=*target->numeric_node();node.clock_node=-1;
        if(node.kind=="SignalGenerator" || node.kind=="Mixer" || node.kind=="PcmResampler"){
            const auto graph=numeric_graph();
            for(const auto& remaining:graph.configuration_bindings)
                if(remaining.to==node.id && remaining.input==domain::ConfigurationRole::SampleClock)node.clock_node=remaining.from;
        }
        target->set_numeric_node(node);
    }
}
bool NumericEditorGraphModel::deleteConnection(QtNodes::ConnectionId edge) {
    const auto binding=configuration_binding(edge);
    if(!DataFlowGraphModel::deleteConnection(edge))return false;
    refresh_connected_dc(edge.outNodeId);_edge_policies.erase(edge_key(edge));
    if(binding && !_atomic_edit_in_progress){
        clear_removed_clock_binding(edge);
        // Removing a dependency keeps the numeric settings but makes its
        // assignment unavailable immediately, including physical axis state.
        emit connection_diagnostics_changed();emit atomic_edit_finished();
    }
    return true;
}
QJsonObject NumericEditorGraphModel::connectionUndoState(QtNodes::ConnectionId edge) const {
    if(connectionExists(edge))return {};
    const bool configuration=bool(configuration_binding(edge));
    const auto original=numeric_graph();auto graph=original;
    if(!(configuration?adapt_configuration(edge,graph):adapt_connection_clock(edge,graph)))return {};
    QJsonArray before,after,before_clocks,after_clocks;
    if(!append_changed_node_state(original,graph,before,after,before_clocks,after_clocks))return {};
    return {{"before",before},{"after",after},{"before_clocks",before_clocks},{"after_clocks",after_clocks},{"configuration",configuration}};
}
QJsonObject NumericEditorGraphModel::saveConnection(const QtNodes::ConnectionId& edge) const {
    auto value=QtNodes::toJson(edge);if(auto binding=configuration_binding(edge))value["configuration"]=int(binding->input);else value["conversion"]=int(connection_policy(edge));return value;
}
void NumericEditorGraphModel::loadConnection(const QJsonObject& value){
    const auto conversion=value.value("conversion");
    if(!conversion.isUndefined() && (!conversion.isDouble() || conversion.toInt(-1)<0 || conversion.toInt(-1)>3 || conversion.toDouble()!=conversion.toInt(-1)))return;
    const auto edge=QtNodes::fromJson(value);if(connectionExists(edge))return;
    _edge_policies[edge_key(edge)]=std::uint8_t(conversion.toInt());addConnection(edge);
    if(!connectionExists(edge))_edge_policies.erase(edge_key(edge));
}
bool NumericEditorGraphModel::is_parameter_connection(QtNodes::ConnectionId edge) {
    const auto* target=delegateModel<AudioNodeModel>(edge.inNodeId);
    return target && target->numeric_node() && !domain::numeric_parameter_key(target->numeric_node()->kind,target->port_id(QtNodes::PortType::In,edge.inPortIndex)).empty();
}
bool NumericEditorGraphModel::undoConnection(QtNodes::ConnectionId edge,const QJsonObject& state){
    if(!connectionExists(edge))return false;
    const auto binding=configuration_binding(edge);
    const bool parameter=is_parameter_connection(edge);
    const auto before=state.value("before").toArray(),after=state.value("after").toArray();
    if(before.size()!=after.size() || before.size()>int(numeric_editor::limits().max_nodes))return deleteConnection(edge);
    auto graph=graph_without_undo_edge(edge);
    const auto before_clocks=state.value("before_clocks").toArray(),after_clocks=state.value("after_clocks").toArray();
    const auto restored=restore_undo_nodes(graph,before,after,before_clocks,after_clocks,bool(binding),parameter);
    if(restored==UndoRestore::DeleteConnection)return deleteConnection(edge);
    if(restored==UndoRestore::InvalidState)return false;
    replace_clock_graph(graph);return true;
}
domain::NumericProcessingGraph NumericEditorGraphModel::graph_without_undo_edge(QtNodes::ConnectionId edge) const {
    auto graph=numeric_graph();
    if(const auto binding=configuration_binding(edge)){
        std::erase_if(graph.configuration_bindings,[&](const auto& value){return value==*binding;});
    }else{
        const auto removed=canonical_edge(edge);
        std::erase_if(graph.edges,[&](const auto& value){return value.from==removed.from && value.to==removed.to && value.input_port==removed.input_port && value.output_port==removed.output_port;});
    }
    return graph;
}
void NumericEditorGraphModel::set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider) {
    _capture_device_provider=std::move(provider);
    for(auto id:allNodeIds())if(auto* node=delegateModel<AudioNodeModel>(id))node->set_capture_device_provider(_capture_device_provider);
    QString error;if(!synchronize_native_sources(&error))emit edit_rejected(error);
}
bool NumericEditorGraphModel::edit_scalar_topology(QtNodes::NodeId id,
    const domain::ConfiguredProcessingNode& value,const QJsonObject& presentation){
    if(!AudioNodeModel::valid_presentation(presentation))return false;
    if(_undo)_undo->beginMacro(ui::text("Edit metric inputs"));
    const bool accepted=edit_node(id,value);
    if(accepted)edit_presentation(id,presentation);
    if(_undo)_undo->endMacro();return accepted;
}
void NumericEditorGraphModel::bind_node_handlers(QtNodes::NodeId id,AudioNodeModel& node){
    node.set_numeric_edit_handler([this,id](const auto& value){return edit_node(id,value);});
    node.set_presentation_edit_handler([this,id](const auto& value){return edit_presentation(id,value);});
    node.set_scalar_topology_handler([this,id](const auto& value,const auto& presentation){return edit_scalar_topology(id,value,presentation);});
    node.set_capture_device_provider(_capture_device_provider);
    node.set_admission_limits(_admission_limits);
    node.set_adapter_edit_handler([this,id](const auto& value){return edit_adapter(id,value);});
}
void NumericEditorGraphModel::bind_node(QtNodes::NodeId id) {
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        connect(node,&AudioNodeModel::diagnostics_changed,this,&NumericEditorGraphModel::connection_diagnostics_changed,Qt::UniqueConnection);
        if(const auto* current=node->numeric_node()){auto copy=*current;copy.id=domain::NodeId(id);node->set_numeric_node(copy);}
        bind_node_handlers(id,*node);
    }
}
void NumericEditorGraphModel::reveal_connected_dc(const domain::NumericProcessingGraph& graph) {
    for(const auto& edge:graph.edges)if(edge.output_port==2 || edge.output_port==3)
        if(nodeExists(QtNodes::NodeId(edge.from)))if(auto* node=delegateModel<AudioNodeModel>(QtNodes::NodeId(edge.from));node && (node->name()=="Fourier" || node->name()=="RealFourier"))node->set_dc_connections_visible(true);
}
void NumericEditorGraphModel::refresh_connected_dc(QtNodes::NodeId id) {
    if(!nodeExists(id))return;
    auto* node=delegateModel<AudioNodeModel>(id);
    if(!node || (node->name()!="Fourier" && node->name()!="RealFourier"))return;
    bool connected=false;
    for(const auto& edge:allConnectionIds(id))if(edge.outNodeId==id && (edge.outPortIndex==2 || edge.outPortIndex==3)){connected=true;break;}
    node->set_dc_connections_visible(connected);
}
void NumericEditorGraphModel::loadNode(const QJsonObject& json){DataFlowGraphModel::loadNode(json);const auto id=QtNodes::NodeId(json.value("id").toInteger());bind_node(id);}
namespace {
void use_unambiguous_mixer_clock(const domain::NumericProcessingGraph& graph,domain::ConfiguredProcessingNode& configured){
    const domain::ConfiguredProcessingNode* clock=nullptr;
    for(const auto& node:graph.nodes)if(node.kind=="SampleClock"){
        if(clock)return;
        clock=&node;
    }
    if(!clock)return;
    const auto* parameters=std::get_if<domain::SampleClockProcessingParameters>(&clock->parameters);
    if(parameters && numeric_editor::clock_node(configured,{parameters->domain,{1,parameters->sample_rate},parameters->sample_rate}))
        configured.clock_node=clock->id;
}
}
void NumericEditorGraphModel::sync_new_audio_source(){
    auto graph=numeric_graph();QString error;
    if(resolve_native_sources(graph,&error))replace_clock_graph(graph);else emit edit_rejected(error);
}
QtNodes::NodeId NumericEditorGraphModel::addNode(QString kind){
    auto id=DataFlowGraphModel::addNode(kind);
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        auto configured=numeric_editor::default_node(kind);
        // Editor default only: materialize an explicit, editable clock reference
        // when there is one unambiguous clock. Never guess among independent clocks.
        if(kind=="Mixer")use_unambiguous_mixer_clock(numeric_graph(),configured);
        node->set_numeric_node(configured);
    }
    bind_node(id);if(auto* model=delegateModel<AudioNodeModel>(id);model && model->numeric_node() && model->numeric_node()->clock_node>=0)DataFlowGraphModel::addConnection(configuration_connection({model->numeric_node()->clock_node,domain::NodeId(id)}));
    if(kind=="AudioSource" && _capture_device_provider)sync_new_audio_source();return id;
}
namespace {
const domain::ProcessingPort* graph_port(const domain::NumericProcessingGraph& graph,domain::NodeId id,domain::PortDirection direction,domain::PortId port){
    const auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& value){return value.id==id;});
    if(node==graph.nodes.end())return nullptr;
    const auto& ports=direction==domain::PortDirection::Input?node->inputs:node->outputs;
    const auto found=std::find_if(ports.begin(),ports.end(),[&](const auto& value){return value.id==port;});
    return found==ports.end()?nullptr:&*found;
}
bool graph_port_changed(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& after,domain::NodeId id,domain::PortDirection direction,domain::PortId port){
    const auto* a=graph_port(before,id,direction,port);const auto* b=graph_port(after,id,direction,port);
    return !a || !b || !signal_contracts::same_contract(a->contract.view(),b->contract.view());
}
bool graph_port_order_changed(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& after,domain::NodeId id,domain::PortDirection direction) {
    const auto previous=std::find_if(before.nodes.begin(),before.nodes.end(),[id](const auto& node){return node.id==id;});
    const auto next=std::find_if(after.nodes.begin(),after.nodes.end(),[id](const auto& node){return node.id==id;});
    if(previous==before.nodes.end() || next==after.nodes.end())return true;
    const auto& left=direction==domain::PortDirection::Input?previous->inputs:previous->outputs;
    const auto& right=direction==domain::PortDirection::Input?next->inputs:next->outputs;
    return left.size()!=right.size() || !std::equal(left.begin(),left.end(),right.begin(),[](const auto& a,const auto& b){return a.id==b.id;});
}
bool retained_graph_edge(const domain::NumericProcessingGraph& graph,const domain::Edge& edge){
    return std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& value){return value.from==edge.from && value.to==edge.to && value.input_port==edge.input_port && value.output_port==edge.output_port;});
}
bool same_configured_node(const domain::ConfiguredProcessingNode* before,const domain::ConfiguredProcessingNode& after){
    return before && before->enabled==after.enabled && before->clock_node==after.clock_node && domain::same_processing_parameters(before->parameters,after.parameters) && same_clock_ports(*before,after);
}
}
void NumericEditorGraphModel::remove_changed_connections(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& after){
    std::vector<QtNodes::ConnectionId> removed;
    for(const auto& edge:before.edges){
        if(!retained_graph_edge(after,edge) || graph_port_changed(before,after,edge.from,domain::PortDirection::Output,edge.output_port) || graph_port_changed(before,after,edge.to,domain::PortDirection::Input,edge.input_port) ||
           graph_port_order_changed(before,after,edge.from,domain::PortDirection::Output) || graph_port_order_changed(before,after,edge.to,domain::PortDirection::Input))removed.push_back(connection_id(edge));
    }
    for(const auto& binding:before.configuration_bindings)
        if(graph_port_order_changed(before,after,binding.from,domain::PortDirection::Output) || graph_port_order_changed(before,after,binding.to,domain::PortDirection::Input))removed.push_back(configuration_connection(binding));
    for(const auto& edge:removed){_edge_policies.erase(edge_key(edge));DataFlowGraphModel::deleteConnection(edge);}
}
void NumericEditorGraphModel::update_graph_nodes(const domain::NumericProcessingGraph& graph){
    for(const auto& value:graph.nodes)if(auto* model=delegateModel<AudioNodeModel>(QtNodes::NodeId(value.id)))
        if(!same_configured_node(model->numeric_node(),value))model->set_numeric_node(value);
}
void NumericEditorGraphModel::restore_data_connections(const domain::NumericProcessingGraph& graph){
    reveal_connected_dc(graph);
    for(const auto& edge:graph.edges){const auto qt=connection_id(edge);_edge_policies[edge_key(qt)]=edge.conversion;if(!connectionExists(qt))DataFlowGraphModel::addConnection(qt);}
    for(const auto id:allNodeIds())refresh_connected_dc(id);
}
void NumericEditorGraphModel::restore_configuration_connections(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& graph){
    for(const auto& binding:before.configuration_bindings)if(std::find(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),binding)==graph.configuration_bindings.end())DataFlowGraphModel::deleteConnection(configuration_connection(binding));
    for(const auto& binding:graph.configuration_bindings){const auto edge=configuration_connection(binding);if(!connectionExists(edge))DataFlowGraphModel::addConnection(edge);}
}
void NumericEditorGraphModel::restore_graph_connections(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& graph){
    restore_data_connections(graph);restore_configuration_connections(before,graph);
}
void NumericEditorGraphModel::replace_clock_graph(const domain::NumericProcessingGraph& graph){
    {
        QScopedValueRollback<bool> transaction(_atomic_edit_in_progress,true);
        QScopedValueRollback<bool> adaptation(_allow_connection_clock_adaptation,false);
        const auto before=numeric_graph();remove_changed_connections(before,graph);update_graph_nodes(graph);restore_graph_connections(before,graph);
    }
    emit connection_diagnostics_changed();emit atomic_edit_finished();
}
bool NumericEditorGraphModel::edit_execution_interval(signal_contracts::Ratio value) {
    if(!domain::valid_execution_interval(value))return false;
    const auto common=std::gcd(std::uint64_t(value.numerator),value.denominator);value={value.numerator/std::int64_t(common),value.denominator/common};
    if(value==_execution_interval)return true;const auto before=_execution_interval;
    auto apply=[this](signal_contracts::Ratio interval){_execution_interval=interval;emit persistence_topology_changed();emit atomic_edit_finished();};
    if(!_undo){apply(value);return true;}
    _undo->push(new GraphParameterEdit(ui::text("Analysis rate, Hz"),[apply,before,value](bool forward){apply(forward?value:before);}));return true;
}
namespace {
bool apply_node_changes(domain::NumericProcessingGraph& graph,
    const std::vector<domain::ConfiguredProcessingNode>& changes){
    for(const auto& changed:changes){
        auto node=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& candidate){return candidate.id==changed.id && candidate.kind==changed.kind;});
        if(node==graph.nodes.end())return false;
        *node=changed;
    }
    return true;
}
}
bool NumericEditorGraphModel::edit_nodes(const std::vector<domain::ConfiguredProcessingNode>& changes,QString* error){
    const auto before=numeric_graph();auto candidate=before;
    if(!apply_node_changes(candidate,changes)){if(error)*error=ui::text("The coefficient route is no longer available");return false;}
    if(!evaluate_node_changes(candidate,error))return false;
    commit_graph_edit(before,candidate,ui::text("Apply frequency profile"));return true;
}
bool NumericEditorGraphModel::evaluate_node_changes(domain::NumericProcessingGraph& candidate,QString* error){
    domain::NumericProcessingGraph evaluated,executable;auto diagnostic=domain::resolve_numeric_parameter_bindings(candidate,numeric_editor::limits(),evaluated,executable);
    if(!diagnostic){if(error)*error=ui::text("Unsupported node settings");return false;}
    if(!draft_preflight(evaluated,error))return false;
    candidate=std::move(evaluated);return true;
}
namespace {
void bind_changed_clock(domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& changed,domain::NodeId prior_clock){
    if(changed.clock_node==prior_clock)return;
    std::erase_if(graph.configuration_bindings,[&](const auto& binding){return binding.to==changed.id && binding.input==domain::ConfigurationRole::SampleClock;});
    if(changed.clock_node>=0)graph.configuration_bindings.push_back({changed.clock_node,changed.id});
}
bool bind_clock_consumers(domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& changed,
    const signal_contracts::ClockContract& clock,std::vector<domain::NodeId>& consumers){
    for(const auto& binding:graph.configuration_bindings){
        if(binding.from!=changed.id || binding.input!=domain::ConfigurationRole::SampleClock)continue;
        auto target=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& node){return node.id==binding.to;});
        if(target==graph.nodes.end())return false;
        if(target->kind=="SignalGenerator" || target->kind=="Mixer" || target->kind=="PcmResampler"){
            if(!numeric_editor::bind_target_clock(*target,clock))return false;
            consumers.push_back(target->id);
        }
    }
    return true;
}
bool resolve_clock_consumers(domain::NumericProcessingGraph& graph,std::span<const domain::NodeId> consumers){
    for(const auto id:consumers){domain::NumericProcessingGraph evaluated,executable;
        if(!domain::resolve_numeric_parameter_bindings(graph,numeric_editor::limits(),evaluated,executable,id))return false;
        graph=std::move(evaluated);}
    return true;
}
bool update_sample_clock_consumers(domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& changed){
    if(changed.kind!="SampleClock")return true;
    const auto clock=synchronization_clock(changed);if(!clock)return false;
    std::vector<domain::NodeId> consumers;
    return bind_clock_consumers(graph,changed,*clock,consumers) &&
        resolve_clock_consumers(graph,std::span<const domain::NodeId>(consumers));
}
bool resolve_edited_configuration(domain::NumericProcessingGraph& graph){
    domain::NumericProcessingGraph configured;
    const bool valid_configuration=bool(domain::resolve_configuration_bindings(graph,numeric_editor::limits(),configured));
    if(valid_configuration)graph=std::move(configured);
    return valid_configuration;
}
bool resolve_edited_parameters(domain::NumericProcessingGraph& graph,domain::NodeId id){
    domain::NumericProcessingGraph evaluated,executable;
    const bool valid_parameters=bool(domain::resolve_numeric_parameter_bindings(graph,numeric_editor::limits(),evaluated,executable,id));
    if(valid_parameters)graph=std::move(evaluated);
    return valid_parameters;
}
bool prepare_parameter_edit(domain::NumericProcessingGraph& graph,domain::ConfiguredProcessingNode changed,domain::NodeId prior_clock){
    if(!domain::derive_processing_node_contracts(changed,numeric_editor::limits()))return false;
    for(auto& node:graph.nodes)if(node.id==changed.id)node=changed;
    bind_changed_clock(graph,changed,prior_clock);
    if(!update_sample_clock_consumers(graph,changed))return false;
    const bool valid_configuration=resolve_edited_configuration(graph);
    const bool valid_parameters=resolve_edited_parameters(graph,changed.id);
    // The edited node's own ports were derived before this transaction. A
    // downstream refusal remains a visible, non-executable draft; keeping that
    // edge allows the user to repair explicit spectral Fs/N/calibration inputs.
    if(changed.kind=="SampleClock" && (!valid_configuration || !valid_parameters))return false;
    return true;
}
}
bool NumericEditorGraphModel::draft_preflight(const domain::NumericProcessingGraph& graph,QString* error) {
    NumericEditorGraphModel preflight(dataModelRegistry());preflight.set_node_validator(_node_validator);preflight.set_admission_limits(_admission_limits);return preflight.set_numeric_graph(graph,error);
}
namespace {
bool same_parameter_edit_graph(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& after){
    if(before.graph_id!=after.graph_id || before.versions.graph!=after.versions.graph ||
       before.versions.processing!=after.versions.processing || before.execution_interval!=after.execution_interval ||
       before.edges!=after.edges || before.configuration_bindings!=after.configuration_bindings ||
       before.nodes.size()!=after.nodes.size())return false;
    for(std::size_t i=0;i<before.nodes.size();++i){
        const auto& old_node=before.nodes[i];const auto& new_node=after.nodes[i];
        if(old_node.id!=new_node.id || old_node.kind!=new_node.kind ||
           !same_configured_node(&old_node,new_node))return false;
    }
    return true;
}
}
void NumericEditorGraphModel::commit_graph_edit(const domain::NumericProcessingGraph& before,const domain::NumericProcessingGraph& after,const QString& title){
    const auto apply=[this,before,after](bool forward){replace_clock_graph(forward?after:before);emit immediate_apply_requested();};
    if(_undo)_undo->push(new GraphParameterEdit(title,apply));else apply(true);
}
namespace {
bool removed_port(const std::vector<domain::ProcessingPort>& ports,domain::PortId id){
    return std::none_of(ports.begin(),ports.end(),[&](const auto& port){return port.id==id;});
}
void remove_invalid_node_edges(domain::NumericProcessingGraph& graph,const domain::ConfiguredProcessingNode& changed){
    std::erase_if(graph.edges,[&](const domain::Edge& edge){
        if(edge.from==changed.id && removed_port(changed.outputs,edge.output_port))return true;
        return edge.to==changed.id && removed_port(changed.inputs,edge.input_port);
    });
}
}
bool NumericEditorGraphModel::validate_generator_edit(const domain::ConfiguredProcessingNode& changed) {
    if(changed.kind=="SignalGenerator"){
        domain::ResolvedProcessingNode resolved;
        if(!domain::resolve_processing_node(changed,numeric_editor::limits(),resolved)){
            emit edit_rejected(ui::text("Unsupported node settings"));return false;
        }
    }
    return true;
}
bool NumericEditorGraphModel::commit_parameter_edit(const domain::NumericProcessingGraph& original,const domain::NumericProcessingGraph& candidate) {
    if(!draft_preflight(candidate)){emit edit_rejected(ui::text("Draft contains a missing endpoint"));return false;}
    if(same_parameter_edit_graph(original,candidate))return true;
    commit_graph_edit(original,candidate,ui::text("Edit parameters"));return true;
}
bool NumericEditorGraphModel::edit_node(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    if(!valid_node_edit_source(id,value))return false;
    const auto* model=delegateModel<AudioNodeModel>(id);
    const auto original=numeric_graph();auto candidate=original;auto changed=value;changed.id=domain::NodeId(id);
    if(!validate_generator_edit(changed))return false;
    remove_invalid_node_edges(candidate,changed);
    if(!prepare_parameter_edit(candidate,std::move(changed),model->numeric_node()->clock_node)){
        emit edit_rejected(ui::text("Unsupported node settings"));return false;
    }
    return commit_parameter_edit(original,candidate);
}
bool NumericEditorGraphModel::valid_node_edit_source(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    const auto* model=delegateModel<AudioNodeModel>(id);
    if(!model || !model->numeric_node() || value.kind!=model->numeric_node()->kind)return false;
    if(value.kind!="AudioSource" || model->adapter().value("device_identity").toString().isEmpty())return true;
    if(numeric_editor::capture_source_kind(value)==numeric_editor::capture_source_kind(*model->numeric_node()))return true;
    emit edit_rejected(ui::text("Clear the selected device before changing the capture source"));
    return false;
}
bool NumericEditorGraphModel::edit_presentation(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);if(!node || !AudioNodeModel::valid_presentation(value) || node->presentation()==value)return false;
    const auto before=node->presentation();
    auto apply=[this,id](const QJsonObject& presentation){if(auto* model=delegateModel<AudioNodeModel>(id))model->set_presentation(presentation);};
    if(!_undo){apply(value);return true;}
    _undo->push(new GraphParameterEdit(ui::text("Edit node display"),
        [apply,before,value](bool forward){apply(forward?value:before);}));return true;
}
bool NumericEditorGraphModel::resolve_adapter_graph(QtNodes::NodeId id,const QJsonObject& adapter,
    domain::NumericProcessingGraph& graph){
    if(!_capture_device_provider)return true;
    QString diagnostic;
    if(resolve_native_source(graph,domain::NodeId(id),adapter,&diagnostic))return true;
    emit edit_rejected(diagnostic);return false;
}
bool NumericEditorGraphModel::edit_adapter(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);QJsonObject checked;
    if(!node || !desktop::decode_numeric_node_adapter(QJsonDocument(value).toJson(QJsonDocument::Compact),checked) || node->adapter()==checked)return false;
    if(const auto error=capture_adapter_error(*node,checked);!error.isEmpty()){emit edit_rejected(error);return false;}
    const auto before=node->adapter();const auto oldGraph=numeric_graph();auto newGraph=oldGraph;
    if(node->name()=="AudioSource" && !resolve_adapter_graph(id,checked,newGraph))return false;
    commit_adapter_edit(id,before,checked,oldGraph,newGraph);return true;
}
QString NumericEditorGraphModel::capture_adapter_error(const AudioNodeModel& node,const QJsonObject& checked) const {
    if(node.name()!="AudioSource" || !node.numeric_node())return {};
    if(const auto error=node_error(*node.numeric_node());!error.isEmpty())return error;
    const auto selected=checked.value("device_identity").toString();
    if(selected.isEmpty() || selected==node.adapter().value("device_identity").toString())return {};
    QString error;
    const auto choices=_capture_device_provider?_capture_device_provider(numeric_editor::capture_source_kind(*node.numeric_node()),&error):QJsonArray{};
    const bool known=std::any_of(choices.begin(),choices.end(),[&](const auto& choice){return choice.toObject().value("id").toString()==selected;});
    return known?QString{}:error.isEmpty()?ui::text("Selected capture device is unavailable"):error;
}
void NumericEditorGraphModel::commit_adapter_edit(QtNodes::NodeId id,const QJsonObject& before,const QJsonObject& after,
    const domain::NumericProcessingGraph& oldGraph,const domain::NumericProcessingGraph& newGraph){
    auto apply=[this,id,before,after,oldGraph,newGraph](bool forward){
        if(auto* model=delegateModel<AudioNodeModel>(id))model->set_adapter(forward?after:before);
        replace_clock_graph(forward?newGraph:oldGraph);
    };
    if(_undo)_undo->push(new GraphParameterEdit(ui::text("Edit source settings"),apply));else apply(true);
}

bool NumericEditorGraphModel::adapt_connection_clock(QtNodes::ConnectionId edge,domain::NumericProcessingGraph& graph) const {
    return numeric_editor::adapt_connection_clock(canonical_edge(edge),graph,[this](const auto& n){return node_error(n).isEmpty();});
}
bool NumericEditorGraphModel::propagate_clock(domain::NumericProcessingGraph& graph,domain::NodeId root) const {
    return numeric_editor::propagate_clock(graph,root,[this](const auto& n){return node_error(n).isEmpty();});
}
