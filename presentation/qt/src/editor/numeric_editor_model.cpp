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
#include "numeric_graph_draft.h"
#include <QScopedValueRollback>

NumericEditorGraphModel::NumericEditorGraphModel(std::shared_ptr<QtNodes::NodeDelegateModelRegistry> registry):DataFlowGraphModel(std::move(registry)) {

}
bool NumericEditorGraphModel::has_numeric_nodes() const {return true;}
QString NumericEditorGraphModel::node_error(const domain::ConfiguredProcessingNode& node) const {
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved))return ui::text("Unsupported node settings");
    if(_node_validator && !_node_validator(node))return ui::text("Device does not support these node settings");
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
bool NumericEditorGraphModel::adapt_configuration(QtNodes::ConnectionId edge,domain::NumericProcessingGraph& graph) const {
    const auto binding=configuration_binding(edge);if(!binding || binding->output!=binding->input)return false;
    auto from=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==binding->from;});auto to=std::find_if(graph.nodes.begin(),graph.nodes.end(),[&](const auto& n){return n.id==binding->to;});
    if(from==graph.nodes.end() || to==graph.nodes.end())return false;
    if(binding->input==domain::ConfigurationRole::SampleClock && (to->kind=="SignalGenerator" || to->kind=="Mixer" || to->kind=="PcmResampler")){
        const auto* clock=std::get_if<domain::SampleClockProcessingParameters>(&from->parameters);if(!clock)return false;
        if(!numeric_editor::bind_target_clock(*to,{clock->domain,{1,clock->sample_rate},clock->sample_rate}))return false;to->clock_node=from->id;
        if(!propagate_clock(graph,to->id))return false;
    }
    graph.configuration_bindings.push_back(*binding);domain::NumericProcessingGraph resolved;
    return bool(domain::resolve_configuration_bindings(graph,numeric_editor::limits(),resolved));
}
bool NumericEditorGraphModel::connectionPossible(QtNodes::ConnectionId edge) const {
    if(!nodeExists(edge.outNodeId) || !nodeExists(edge.inNodeId) || edge.outPortIndex>=nodeData(edge.outNodeId,QtNodes::NodeRole::OutPortCount).toUInt() || edge.inPortIndex>=nodeData(edge.inNodeId,QtNodes::NodeRole::InPortCount).toUInt())return false;
    if(!DataFlowGraphModel::connectionPossible(edge))return false;
    auto* self=const_cast<NumericEditorGraphModel*>(this);
    auto* from=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!from || !to)return false;
    const auto out_role=from->configuration_role(QtNodes::PortType::Out,edge.outPortIndex),in_role=to->configuration_role(QtNodes::PortType::In,edge.inPortIndex);
    if(out_role || in_role){
        if(!out_role || !in_role || *out_role!=*in_role)return false;
        if(!_allow_connection_clock_adaptation)return true;
        auto graph=numeric_graph();return adapt_configuration(edge,graph);
    }
    const auto* a=from->numeric_node();const auto* b=to->numeric_node();
    if(!a || !b || !node_error(*a).isEmpty() || !node_error(*b).isEmpty())return false;
    domain::ResolvedProcessingNode x,y;
    if(!domain::resolve_processing_node(*a,numeric_editor::limits(),x) || !domain::resolve_processing_node(*b,numeric_editor::limits(),y))return false;
    const auto find=[](auto ports,auto id)->const domain::ProcessingPort*{for(const auto& p:ports)if(p.id==id)return &p;return nullptr;};
    const auto* output=find(x.outputs,from->port_id(QtNodes::PortType::Out,edge.outPortIndex));const auto* input=find(y.inputs,to->port_id(QtNodes::PortType::In,edge.inPortIndex));
    if(output && input && numeric_editor::is_graph_view(to->name()) && (!std::holds_alternative<signal_contracts::NumericContract>(output->contract.view()) || !numeric_editor::view_accepts(to->name(),std::get<signal_contracts::NumericContract>(output->contract.view()))))return false;
    if(output && input && numeric_editor::is_graph_view(to->name()) && !signal_contracts::same_contract(output->contract.view(),input->contract.view())){
        if(!_allow_connection_clock_adaptation)return false;
        auto graph=numeric_graph();return adapt_connection_clock(edge,graph);
    }
    if(output && input && (domain::processing_ports_compatible(*output,*input) || domain::processing_ports_dtype_convertible(*output,*input)))return true;
    if(!_allow_connection_clock_adaptation)return false;
    auto graph=numeric_graph();return adapt_connection_clock(edge,graph);
}
namespace {
auto edge_key(QtNodes::ConnectionId e){return std::make_tuple(e.outNodeId,e.outPortIndex,e.inNodeId,e.inPortIndex);}
bool same_clock_ports(const domain::ConfiguredProcessingNode& a,const domain::ConfiguredProcessingNode& b){
    for(auto pair:{std::pair{&a.inputs,&b.inputs},std::pair{&a.outputs,&b.outputs}}){
        if(pair.first->size()!=pair.second->size())return false;
        for(std::size_t i=0;i<pair.first->size();++i)if((*pair.first)[i].id!=(*pair.second)[i].id || !signal_contracts::same_contract((*pair.first)[i].contract.view(),(*pair.second)[i].contract.view()))return false;
    }
    return true;
}
}
std::uint8_t NumericEditorGraphModel::connection_policy(QtNodes::ConnectionId edge) const {
    const auto it=_edge_policies.find(edge_key(edge));return it==_edge_policies.end()?0:it->second;
}
QString NumericEditorGraphModel::connection_status(QtNodes::ConnectionId edge) const {
    if(configuration_binding(edge))return ui::text("Configuration dependency");
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node())return ui::text("Unavailable");
    const auto& outputs=a->numeric_node()->outputs;const auto& inputs=b->numeric_node()->inputs;
    const auto out=std::find_if(outputs.begin(),outputs.end(),[&](const auto& p){return p.id==a->port_id(QtNodes::PortType::Out,edge.outPortIndex);});
    const auto in=std::find_if(inputs.begin(),inputs.end(),[&](const auto& p){return p.id==b->port_id(QtNodes::PortType::In,edge.inPortIndex);});
    if(out==outputs.end() || in==inputs.end())return ui::text("Unavailable");
    const auto& output=*out;const auto& input=*in;
    if(domain::processing_ports_compatible(output,input))return ui::text("Exact connection");
    if(!domain::processing_ports_dtype_convertible(output,input))return ui::text("Incompatible connection");
    const auto from=std::get<signal_contracts::NumericContract>(output.contract.view()).dtype,to=std::get<signal_contracts::NumericContract>(input.contract.view()).dtype;
    return ui::text(domain::dtype_conversion_is_exact(from,to)?"Exact dtype widening":"Potential loss: range / rounding / precision")+"\n"+ui::text("Nonfinite values are rejected");
}
bool NumericEditorGraphModel::edit_connection_policy(QtNodes::ConnectionId edge,std::uint8_t value) {
    if(value>3 || configuration_binding(edge) || !connectionExists(edge) || connection_policy(edge)==value)return false;
    const auto before=connection_policy(edge);
    auto apply=[this,edge](std::uint8_t v){_edge_policies[edge_key(edge)]=v;emit nodeUpdated(edge.inNodeId);};
    class Edit final:public QUndoCommand{std::function<void(bool)> action;public:Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Connection conversion"));}void redo()override{action(true);}void undo()override{action(false);}};
    if(_undo)_undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));else apply(value);
    return true;
}
QString NumericEditorGraphModel::connection_failure(QtNodes::ConnectionId edge) const {
    if(configuration_binding(edge))return {};
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(a){const auto error=a->diagnostic_failure(domain::PortDirection::Output,a->port_id(QtNodes::PortType::Out,edge.outPortIndex));if(!error.isEmpty())return error;}
    return b?b->diagnostic_failure(domain::PortDirection::Input,b->port_id(QtNodes::PortType::In,edge.inPortIndex)):QString{};
}
int NumericEditorGraphModel::connection_visual_state(QtNodes::ConnectionId edge) const {
    if(configuration_binding(edge))return 0;
    if(!connection_failure(edge).isEmpty())return 3;
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node())return 3;
    const auto& outputs=a->numeric_node()->outputs;const auto& inputs=b->numeric_node()->inputs;
    if(edge.outPortIndex>=outputs.size() || edge.inPortIndex>=inputs.size())return 3;
    const auto& out=outputs[edge.outPortIndex];const auto& in=inputs[edge.inPortIndex];
    if(domain::processing_ports_compatible(out,in))return 0;
    if(!domain::processing_ports_dtype_convertible(out,in))return 3;
    return domain::dtype_conversion_is_exact(std::get<signal_contracts::NumericContract>(out.contract.view()).dtype,std::get<signal_contracts::NumericContract>(in.contract.view()).dtype)?1:2;
}
QString NumericEditorGraphModel::connection_effects(QtNodes::ConnectionId edge) const {
    auto* self=const_cast<NumericEditorGraphModel*>(this);auto* a=self->delegateModel<AudioNodeModel>(edge.outNodeId);auto* b=self->delegateModel<AudioNodeModel>(edge.inNodeId);
    if(!a || !b || !a->numeric_node() || !b->numeric_node() || edge.outPortIndex>=a->numeric_node()->outputs.size() || edge.inPortIndex>=b->numeric_node()->inputs.size())return {};
    const auto out=a->numeric_node()->outputs[edge.outPortIndex].contract.view(),in=b->numeric_node()->inputs[edge.inPortIndex].contract.view();
    const auto* x=std::get_if<signal_contracts::NumericContract>(&out);const auto* y=std::get_if<signal_contracts::NumericContract>(&in);
    return x && y?numeric_editor::conversion_effects(x->dtype,y->dtype):QString{};
}
void NumericEditorGraphModel::addConnection(QtNodes::ConnectionId edge) {
    if(connectionExists(edge) || !connectionPossible(edge))return;
    auto* from=delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=delegateModel<AudioNodeModel>(edge.inNodeId);
    if(configuration_binding(edge)){
        // Canonical import already prepared every contract together. Replaying
        // configuration wires must not re-clock partial downstream branches.
        if(!_allow_connection_clock_adaptation){DataFlowGraphModel::addConnection(edge);return;}
        auto graph=numeric_graph();if(adapt_configuration(edge,graph))replace_clock_graph(graph);return;
    }
    const auto& output=from->numeric_node()->outputs[edge.outPortIndex];const auto& input=to->numeric_node()->inputs[edge.inPortIndex];
    if(numeric_editor::is_graph_view(to->name()) && !signal_contracts::same_contract(output.contract.view(),input.contract.view())){
        auto graph=numeric_graph();if(adapt_connection_clock(edge,graph))replace_clock_graph(graph);return;
    }
    if(domain::processing_ports_compatible(output,input) || domain::processing_ports_dtype_convertible(output,input)){DataFlowGraphModel::addConnection(edge);return;}
    auto graph=numeric_graph();if(adapt_connection_clock(edge,graph))replace_clock_graph(graph);
}
bool NumericEditorGraphModel::deleteConnection(QtNodes::ConnectionId edge) {
    const auto binding=configuration_binding(edge);
    if(!DataFlowGraphModel::deleteConnection(edge))return false;
    _edge_policies.erase(edge_key(edge));
    if(binding && !_atomic_edit_in_progress){
        if(auto* target=delegateModel<AudioNodeModel>(edge.inNodeId);target && target->numeric_node()){
            auto node=*target->numeric_node();node.clock_node=-1;
            if(node.kind=="SignalGenerator" || node.kind=="Mixer" || node.kind=="PcmResampler"){
                const auto graph=numeric_graph();for(const auto& remaining:graph.configuration_bindings)if(remaining.to==node.id && remaining.input==domain::ConfigurationRole::SampleClock)node.clock_node=remaining.from;
            }
            target->set_numeric_node(node);
        }
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
    for(std::size_t i=0;i<graph.nodes.size();++i)if(!same_clock_ports(original.nodes[i],graph.nodes[i]) || original.nodes[i].clock_node!=graph.nodes[i].clock_node || !domain::same_processing_parameters(original.nodes[i].parameters,graph.nodes[i].parameters)){
        QByteArray old_bytes,new_bytes;
        if(!desktop::encode_numeric_node(original.nodes[i],old_bytes) || !desktop::encode_numeric_node(graph.nodes[i],new_bytes))return {};
        before.append(QString::fromUtf8(old_bytes));after.append(QString::fromUtf8(new_bytes));
        before_clocks.append(QString::number(original.nodes[i].clock_node));after_clocks.append(QString::number(graph.nodes[i].clock_node));
    }
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
bool NumericEditorGraphModel::undoConnection(QtNodes::ConnectionId edge,const QJsonObject& state){
    if(!connectionExists(edge))return false;
    const auto binding=configuration_binding(edge);
    const auto before=state.value("before").toArray(),after=state.value("after").toArray();
    if(before.size()!=after.size() || before.size()>int(numeric_editor::limits().max_nodes))return deleteConnection(edge);
    auto graph=numeric_graph();
    if(binding)std::erase_if(graph.configuration_bindings,[&](const auto& value){return value==*binding;});
    else {const auto removed=canonical_edge(edge);std::erase_if(graph.edges,[&](const auto& e){return e.from==removed.from && e.to==removed.to && e.input_port==removed.input_port && e.output_port==removed.output_port;});}
    const auto before_clocks=state.value("before_clocks").toArray(),after_clocks=state.value("after_clocks").toArray();
    for(int i=0;i<after.size();++i){
        domain::ConfiguredProcessingNode old_node,new_node;
        if(!desktop::decode_numeric_node(before[i].toString().toUtf8(),old_node) || !desktop::decode_numeric_node(after[i].toString().toUtf8(),new_node) || old_node.id!=new_node.id)return deleteConnection(edge);
        if(binding){
            if(before_clocks.size()!=before.size() || after_clocks.size()!=after.size())return false;
            bool old_ok=false,new_ok=false;old_node.clock_node=before_clocks[i].toString().toLongLong(&old_ok);new_node.clock_node=after_clocks[i].toString().toLongLong(&new_ok);if(!old_ok || !new_ok)return false;
        }
        for(auto& node:graph.nodes)if(node.id==new_node.id && same_clock_ports(node,new_node)){
            // Preserve later parameter/presentation edits; only roll back the
            // contracts introduced by this connection, while still unchanged.
            node.inputs=old_node.inputs;node.outputs=old_node.outputs;
            if(binding){if(node.clock_node==new_node.clock_node)node.clock_node=old_node.clock_node;if(domain::same_processing_parameters(node.parameters,new_node.parameters))node.parameters=old_node.parameters;}
        }
    }
    bool compatible=true;
    for(const auto& e:graph.edges){const domain::ProcessingPort *out=nullptr,*in=nullptr;
        for(const auto& n:graph.nodes){if(n.id==e.from)for(const auto& p:n.outputs)if(p.id==e.output_port)out=&p;if(n.id==e.to)for(const auto& p:n.inputs)if(p.id==e.input_port)in=&p;}
        compatible=compatible && out && in && (domain::processing_ports_compatible(*out,*in) || domain::processing_ports_dtype_convertible(*out,*in));
    }
    if(!compatible)return deleteConnection(edge);
    replace_clock_graph(graph);return true;
}
void NumericEditorGraphModel::set_capture_device_provider(std::function<QJsonArray(int,QString*)> provider) {
    _capture_device_provider=std::move(provider);
    for(auto id:allNodeIds())if(auto* node=delegateModel<AudioNodeModel>(id))node->set_capture_device_provider(_capture_device_provider);
    QString error;if(!synchronize_native_sources(&error))emit edit_rejected(error);
}
void NumericEditorGraphModel::bind_node(QtNodes::NodeId id) {
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        connect(node,&AudioNodeModel::diagnostics_changed,this,&NumericEditorGraphModel::connection_diagnostics_changed,Qt::UniqueConnection);
        if(const auto* current=node->numeric_node()){auto copy=*current;copy.id=domain::NodeId(id);node->set_numeric_node(copy);}
        node->set_numeric_edit_handler([this,id](const auto& value){return edit_node(id,value);});
        node->set_presentation_edit_handler([this,id](const auto& value){return edit_presentation(id,value);});
        node->set_scalar_topology_handler([this,id](const auto& value,const auto& presentation){
            if(!AudioNodeModel::valid_presentation(presentation))return false;
            if(_undo)_undo->beginMacro(ui::text("Edit metric inputs"));
            const bool accepted=edit_node(id,value);
            if(accepted)edit_presentation(id,presentation);
            if(_undo)_undo->endMacro();return accepted;
        });
        node->set_capture_device_provider(_capture_device_provider);
        node->set_adapter_edit_handler([this,id](const auto& value){return edit_adapter(id,value);});
    }
}
void NumericEditorGraphModel::loadNode(const QJsonObject& json){DataFlowGraphModel::loadNode(json);const auto id=QtNodes::NodeId(json.value("id").toInteger());bind_node(id);}
QtNodes::NodeId NumericEditorGraphModel::addNode(QString kind){
    auto id=DataFlowGraphModel::addNode(kind);
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        auto configured=numeric_editor::default_node(kind);
        // Editor default only: materialize an explicit, editable clock reference
        // when there is one unambiguous clock. Never guess among independent clocks.
        if(kind=="SignalGenerator" || kind=="Mixer"){
            const auto graph=numeric_graph();const domain::ConfiguredProcessingNode* clock=nullptr;bool ambiguous=false;
            for(const auto& n:graph.nodes)if(n.kind=="SampleClock"){if(clock){ambiguous=true;break;}clock=&n;}
            if(clock && !ambiguous)if(const auto* p=std::get_if<domain::SampleClockProcessingParameters>(&clock->parameters))
                if(numeric_editor::clock_node(configured,{p->domain,{1,p->sample_rate},p->sample_rate}))configured.clock_node=clock->id;
        }
        node->set_numeric_node(configured);
    }
    bind_node(id);if(auto* model=delegateModel<AudioNodeModel>(id);model && model->numeric_node() && model->numeric_node()->clock_node>=0)DataFlowGraphModel::addConnection(configuration_connection({model->numeric_node()->clock_node,domain::NodeId(id)}));
    if(kind=="AudioSource" && _capture_device_provider){auto graph=numeric_graph();QString error;if(resolve_native_sources(graph,&error))replace_clock_graph(graph);else emit edit_rejected(error);}return id;
}
void NumericEditorGraphModel::replace_node(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    if(auto* node=delegateModel<AudioNodeModel>(id)){
        const bool nested=_atomic_edit_in_progress;
        {
        QScopedValueRollback<bool> transaction(_atomic_edit_in_progress,true);
        // QtNodes shifts higher port indices on deletion. Our IDs are stable:
        // reconnect only the original endpoints after the structural update.
        std::vector<domain::Edge> edges;std::vector<domain::ConfigurationBinding> bindings;for(auto edge:allConnectionIds(id)){if(auto b=configuration_binding(edge))bindings.push_back(*b);else edges.push_back(canonical_edge(edge));_edge_policies.erase(edge_key(edge));DataFlowGraphModel::deleteConnection(edge);}
        node->set_numeric_node(value);
        for(const auto& edge:edges){const auto qt=connection_id(edge);if(connectionPossible(qt)){_edge_policies[edge_key(qt)]=edge.conversion;addConnection(qt);}}
        for(const auto& b:bindings){const auto qt=configuration_connection(b);if(!connectionExists(qt))DataFlowGraphModel::addConnection(qt);}
        }
        // Parameter-only edits also temporarily detach configuration wires.
        // Publish only the complete restored graph, never an intermediate
        // snapshot missing the edited node's clock or coordinate dependencies.
        if(!nested){emit connection_diagnostics_changed();emit atomic_edit_finished();}
    }
}
void NumericEditorGraphModel::replace_clock_graph(const domain::NumericProcessingGraph& graph){
    {
    QScopedValueRollback<bool> transaction(_atomic_edit_in_progress,true);
    // Preserve delegate objects, preferences, layout and unrelated branches.
    // Reconnect only endpoints whose actual port contract changes, before
    // publishing the preflighted graph as one editor command.
    const auto current_graph=numeric_graph();
    const auto desired_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port)->const domain::ProcessingPort*{
        for(const auto& node:graph.nodes)if(node.id==id)for(const auto& p:direction==domain::PortDirection::Input?node.inputs:node.outputs)if(p.id==port)return &p;return nullptr;
    };
    const auto current_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port)->const domain::ProcessingPort*{
        for(const auto& node:current_graph.nodes)if(node.id==id)for(const auto& p:direction==domain::PortDirection::Input?node.inputs:node.outputs)if(p.id==port)return &p;return nullptr;
    };
    const auto changed_port=[&](domain::NodeId id,domain::PortDirection direction,domain::PortId port){
        const auto* before=current_port(id,direction,port);const auto* after=desired_port(id,direction,port);
        return !before || !after || !signal_contracts::same_contract(before->contract.view(),after->contract.view());
    };
    std::vector<QtNodes::ConnectionId> removed;
    for(const auto& edge:current_graph.edges){
        const bool retained=std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& e){return e.from==edge.from && e.to==edge.to && e.input_port==edge.input_port && e.output_port==edge.output_port;});
        if(!retained || changed_port(edge.from,domain::PortDirection::Output,edge.output_port) || changed_port(edge.to,domain::PortDirection::Input,edge.input_port))removed.push_back(connection_id(edge));
    }
    for(const auto& edge:removed){_edge_policies.erase(edge_key(edge));DataFlowGraphModel::deleteConnection(edge);}
    for(const auto& value:graph.nodes)if(auto* node=delegateModel<AudioNodeModel>(QtNodes::NodeId(value.id))){
        const auto* current=node->numeric_node();
        bool same=current && current->enabled==value.enabled && current->clock_node==value.clock_node && domain::same_processing_parameters(current->parameters,value.parameters);
        for(auto pair:{std::pair{current?&current->inputs:nullptr,&value.inputs},std::pair{current?&current->outputs:nullptr,&value.outputs}}){
            if(!pair.first || pair.first->size()!=pair.second->size()){same=false;continue;}
            for(std::size_t i=0;i<pair.first->size();++i)if((*pair.first)[i].id!=(*pair.second)[i].id || !signal_contracts::same_contract((*pair.first)[i].contract.view(),(*pair.second)[i].contract.view()))same=false;
        }
        if(!same)node->set_numeric_node(value);
    }
    for(const auto& edge:graph.edges){const auto qt=connection_id(edge);_edge_policies[edge_key(qt)]=edge.conversion;if(!connectionExists(qt))DataFlowGraphModel::addConnection(qt);}
    for(const auto& b:current_graph.configuration_bindings)if(std::find(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),b)==graph.configuration_bindings.end())DataFlowGraphModel::deleteConnection(configuration_connection(b));
    for(const auto& b:graph.configuration_bindings){const auto edge=configuration_connection(b);if(!connectionExists(edge))DataFlowGraphModel::addConnection(edge);}
    }
    emit connection_diagnostics_changed();
    emit atomic_edit_finished();
}
bool NumericEditorGraphModel::edit_execution_interval(signal_contracts::Ratio value) {
    if(!domain::valid_execution_interval(value))return false;
    auto common=std::gcd(std::uint64_t(value.numerator),value.denominator);value.numerator/=std::int64_t(common);value.denominator/=common;
    if(value==_execution_interval)return true;const auto before=_execution_interval;
    auto apply=[this](signal_contracts::Ratio interval){_execution_interval=interval;emit atomic_edit_finished();};
    if(!_undo){apply(value);return true;}
    class Edit final:public QUndoCommand {std::function<void(bool)> action;public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Analysis rate, Hz"));}void redo()override{action(true);}void undo()override{action(false);}};
    _undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));return true;
}
bool NumericEditorGraphModel::edit_node(QtNodes::NodeId id,const domain::ConfiguredProcessingNode& value){
    auto* node=delegateModel<AudioNodeModel>(id);if(!node || !node->numeric_node() || value.kind!=node->name().toStdString())return false;
    auto before=*node->numeric_node(),after=value;after.id=domain::NodeId(id);
    if(after.kind=="IndexExtract"){
        if(after.inputs.size()!=1)return false;const auto contract=after.inputs[0].contract.view();const auto* input=std::get_if<signal_contracts::NumericContract>(&contract);
        if(!input || !numeric_editor::adopt_extract_input(after,*input))return false;
    }
    auto reject=[&](const QString& error){emit edit_rejected(error);return false;};
    if(after.kind=="SampleClock" || after.clock_node!=before.clock_node) {
        if(const auto error=node_error(after);!error.isEmpty())return reject(QString::fromStdString(after.kind)+": "+error);
        auto original=numeric_graph(),candidate=original;for(auto& n:candidate.nodes)if(n.id==after.id)n=after;
        if(after.clock_node!=before.clock_node){std::erase_if(candidate.configuration_bindings,[&](const auto& b){return b.to==after.id && b.input==domain::ConfigurationRole::SampleClock;});if(after.clock_node>=0)candidate.configuration_bindings.push_back({after.clock_node,after.id});}
        std::vector<domain::NodeId> changed;
        for(auto& n:candidate.nodes)if(n.clock_node>=0 && (n.id==after.id || n.clock_node==after.id)) {
            const domain::SampleClockProcessingParameters* clock=nullptr;
            for(const auto& source:candidate.nodes)if(source.id==n.clock_node)clock=std::get_if<domain::SampleClockProcessingParameters>(&source.parameters);
            if(!clock)return reject(ui::text("Connected nodes do not support this sample rate"));
            if(!numeric_editor::bind_target_clock(n,{clock->domain,{1,clock->sample_rate},clock->sample_rate}))return false;changed.push_back(n.id);
        }
        for(auto n:changed)if(!propagate_clock(candidate,n))return reject(ui::text("Connected nodes do not support this sample rate"));
        for(const auto& n:candidate.nodes)if(n.id==after.id || std::find(changed.begin(),changed.end(),n.id)!=changed.end())
            if(const auto error=node_error(n);!error.isEmpty())return reject(QString::fromStdString(n.kind)+": "+error);
        if(!_undo){replace_clock_graph(candidate);return true;}
        class ClockEdit final:public QUndoCommand {std::function<void(bool)> action;public:explicit ClockEdit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit sample rate"));}void redo()override{action(true);}void undo()override{action(false);}};
        _undo->push(new ClockEdit([this,original,candidate](bool forward){replace_clock_graph(forward?candidate:original);}));return true;
    }

    if((after.kind=="AudioSource" || after.kind=="TestSignal" || after.kind=="PcmResampler") && !before.outputs.empty() && !after.outputs.empty()){
        const auto old_data=before.outputs[0].contract.view(),new_data=after.outputs[0].contract.view();
        const auto* old_clock=std::get_if<signal_contracts::NumericContract>(&old_data);const auto* new_clock=std::get_if<signal_contracts::NumericContract>(&new_data);
        if(old_clock && new_clock && (old_clock->clock.sample_rate!=new_clock->clock.sample_rate || old_clock->clock.tick_seconds!=new_clock->clock.tick_seconds)){
            if(after.kind=="AudioSource" && numeric_editor::capture_source_kind(before)!=numeric_editor::capture_source_kind(after))return reject(ui::text("Change capture source and sample rate separately"));
            auto original=numeric_graph(),candidate=original;for(auto& n:candidate.nodes)if(n.id==domain::NodeId(id))n=after;
            if(!propagate_clock(candidate,domain::NodeId(id)))return reject(ui::text("Connected nodes do not support this sample rate"));
            if(!_undo){replace_clock_graph(candidate);return true;}
            class RateEdit final:public QUndoCommand{std::function<void(bool)> action;public:explicit RateEdit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit sample rate"));}void redo()override{action(true);}void undo()override{action(false);}};
            _undo->push(new RateEdit([this,original,candidate](bool forward){replace_clock_graph(forward?candidate:original);}));return true;
        }
    }
    if(const auto error=node_error(after);!error.isEmpty())return reject(QString::fromStdString(after.kind)+": "+error);
    if(after.kind=="AudioSource" && numeric_editor::capture_source_kind(before)!=numeric_editor::capture_source_kind(after)){
        const auto selected=node->adapter().value("device_identity").toString();
        if(!selected.isEmpty()){
            QString error;const auto choices=_capture_device_provider?_capture_device_provider(numeric_editor::capture_source_kind(after),&error):QJsonArray{};
            if(std::none_of(choices.begin(),choices.end(),[&](const auto& choice){return choice.toObject().value("id").toString()==selected;}))
                return reject(ui::text("Choose system default before changing capture source"));
        }
    }
    // Views describe their producer's numeric storage. Editing producer shape,
    // dtype or coordinates updates those sinks in the same undo transaction.
    auto original=numeric_graph(),candidate=original;bool adapted_views=false;
    for(auto& n:candidate.nodes)if(n.id==domain::NodeId(id))n=after;
    for(const auto& edge:candidate.edges)if(edge.from==domain::NodeId(id)){
        const auto output=std::find_if(after.outputs.begin(),after.outputs.end(),[&](const auto& p){return p.id==edge.output_port;});
        if(output==after.outputs.end() || !std::holds_alternative<signal_contracts::NumericContract>(output->contract.view()))continue;
        for(auto& target:candidate.nodes)if(target.id==edge.to && numeric_editor::is_graph_view(QString::fromStdString(target.kind)))
            for(auto& input:target.inputs)if(input.id==edge.input_port && !signal_contracts::same_contract(input.contract.view(),output->contract.view())){input.contract=output->contract;adapted_views=true;}
    }
    if(adapted_views){
        NumericEditorGraphModel preflight(dataModelRegistry());preflight.set_node_validator(_node_validator);
        if(!preflight.set_numeric_graph(candidate))return reject(ui::text("Disconnect affected ports before changing their contract"));
        if(!_undo){replace_clock_graph(candidate);return true;}
        class ViewEdit final:public QUndoCommand{std::function<void(bool)> action;public:explicit ViewEdit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit parameters"));}void redo()override{action(true);}void undo()override{action(false);}};
        _undo->push(new ViewEdit([this,original,candidate](bool forward){replace_clock_graph(forward?candidate:original);}));return true;
    }
    if(((before.kind=="InputSelector"&&before.outputs.size()!=after.outputs.size()) || (before.kind=="Switch" && std::get<domain::RouteProcessingParameters>(before.parameters).channels_per_stream!=std::get<domain::RouteProcessingParameters>(after.parameters).channels_per_stream))&&!allConnectionIds(id).empty())
        return reject(ui::text("Disconnect affected ports before changing their contract"));
    for(const auto edge:allConnectionIds(id)){
        if(configuration_binding(edge))continue;
        const auto* other=delegateModel<AudioNodeModel>(edge.outNodeId==id?edge.inNodeId:edge.outNodeId);
        if(!other || !other->numeric_node())return reject(ui::text("Disconnect affected ports before changing their contract"));
        const auto& from=edge.outNodeId==id?after:*other->numeric_node();const auto& to=edge.inNodeId==id?after:*other->numeric_node();
        const auto find=[](const auto& ports,auto index)->const domain::ProcessingPort*{for(const auto& p:ports)if(p.id==index)return &p;return nullptr;};
        const auto stable=canonical_edge(edge);const auto* output=find(from.outputs,stable.output_port);const auto* input=find(to.inputs,stable.input_port);
        if(!input && edge.inNodeId==id && (after.kind=="InputSelector" || after.kind=="ScalarView"))continue;
        if(!output || !input || (!domain::processing_ports_compatible(*output,*input) && !domain::processing_ports_dtype_convertible(*output,*input)))return reject(ui::text("Disconnect affected ports before changing their contract"));
    }
    auto same_ports=[](const auto& a,const auto& b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i)if(a[i].id!=b[i].id || a[i].direction!=b[i].direction || !a[i].contract || !b[i].contract || !signal_contracts::same_contract(a[i].contract.view(),b[i].contract.view()))return false;return true;};
    if(before.enabled==after.enabled && domain::same_processing_parameters(before.parameters,after.parameters) && same_ports(before.inputs,after.inputs) && same_ports(before.outputs,after.outputs))return false;
    if(!_undo){replace_node(id,after);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit parameters"));}void redo() override{action(true);}void undo() override{action(false);}
    };
    std::vector<domain::Edge> edges;for(auto e:allConnectionIds(id))if(!configuration_binding(e))edges.push_back(canonical_edge(e));
    _undo->push(new Edit([this,id,before,after,edges](bool forward){
        {
        QScopedValueRollback<bool> transaction(_atomic_edit_in_progress,true);
        replace_node(id,forward?after:before);
        for(const auto& e:edges){const auto qt=connection_id(e);if(connectionPossible(qt)){_edge_policies[edge_key(qt)]=e.conversion;addConnection(qt);}}
        }
        emit connection_diagnostics_changed();emit atomic_edit_finished();
    }));return true;
}
bool NumericEditorGraphModel::edit_presentation(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);if(!node || !AudioNodeModel::valid_presentation(value) || node->presentation()==value)return false;
    const auto before=node->presentation();
    auto apply=[this,id](const QJsonObject& presentation){if(auto* model=delegateModel<AudioNodeModel>(id))model->set_presentation(presentation);};
    if(!_undo){apply(value);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit node display"));}void redo() override{action(true);}void undo() override{action(false);}
    };
    _undo->push(new Edit([apply,before,value](bool forward){apply(forward?value:before);}));return true;
}
bool NumericEditorGraphModel::edit_adapter(QtNodes::NodeId id,const QJsonObject& value) {
    auto* node=delegateModel<AudioNodeModel>(id);QJsonObject checked;
    if(!node || !desktop::decode_numeric_node_adapter(QJsonDocument(value).toJson(QJsonDocument::Compact),checked) || node->adapter()==checked)return false;
    if(node->name()=="AudioSource" && node->numeric_node()){
        auto reject=[&](const QString& error){emit edit_rejected(error);return false;};
        if(const auto error=node_error(*node->numeric_node());!error.isEmpty())return reject(error);
        const auto selected=checked.value("device_identity").toString();
        if(!selected.isEmpty() && selected!=node->adapter().value("device_identity").toString()){
            QString error;const auto choices=_capture_device_provider?_capture_device_provider(numeric_editor::capture_source_kind(*node->numeric_node()),&error):QJsonArray{};
            const bool known=std::any_of(choices.begin(),choices.end(),[&](const auto& choice){return choice.toObject().value("id").toString()==selected;});
            if(!known)return reject(error.isEmpty()?ui::text("Selected capture device is unavailable"):error);
        }
    }
    const auto before=node->adapter();const auto oldGraph=numeric_graph();auto newGraph=oldGraph;
    if(node->name()=="AudioSource" && _capture_device_provider) {
        QString diagnostic;const auto choices=_capture_device_provider(numeric_editor::capture_source_kind(*node->numeric_node()),&diagnostic);
        const auto wanted=checked.value("device_identity").toString();QJsonObject chosen;
        for(const auto& value:choices){const auto entry=value.toObject();if(wanted.isEmpty()?entry.value("is_default").toBool():entry.value("id").toString()==wanted){chosen=entry;break;}}
        if(chosen.isEmpty()&&wanted.isEmpty()&&!choices.isEmpty())chosen=choices.first().toObject();
        const int count=chosen.value("native_channels").toInt(),fs=chosen.value("native_sample_rate").toInt();
        if(count>0 && fs>0 && !desktop::resolve_numeric_source_format(newGraph,domain::NodeId(id),std::uint32_t(fs),std::uint32_t(count),&diagnostic)){
            emit edit_rejected(diagnostic);return false;
        }
    }
    auto apply=[this,id](const QJsonObject& adapter,const domain::NumericProcessingGraph& graph){
        if(auto* model=delegateModel<AudioNodeModel>(id))model->set_adapter(adapter);replace_clock_graph(graph);
    };
    if(!_undo){apply(checked,newGraph);return true;}
    class Edit final:public QUndoCommand {
        std::function<void(bool)> action;
    public:explicit Edit(std::function<void(bool)> a):action(std::move(a)){setText(ui::text("Edit source settings"));}void redo()override{action(true);}void undo()override{action(false);}
    };
    _undo->push(new Edit([apply,before,checked,oldGraph,newGraph](bool forward){apply(forward?checked:before,forward?newGraph:oldGraph);}));return true;
}

bool NumericEditorGraphModel::adapt_connection_clock(QtNodes::ConnectionId edge,domain::NumericProcessingGraph& graph) const {
    return numeric_editor::adapt_connection_clock(canonical_edge(edge),graph,[this](const auto& n){return node_error(n).isEmpty();});
}
bool NumericEditorGraphModel::propagate_clock(domain::NumericProcessingGraph& graph,domain::NodeId root) const {
    return numeric_editor::propagate_clock(graph,root,[this](const auto& n){return node_error(n).isEmpty();});
}
