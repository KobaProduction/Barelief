#include "core_application_state.h"
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <utility>
namespace {
void capture_placement(desktop::NumericDraftPresentation& out,NumericEditorGraphModel& model,unsigned int id,bool position,bool adapter) {
    auto* node=model.delegateModel<AudioNodeModel>(id);if(!node)return;
    const auto key=QString::number(id);
    if(position) {
        const auto point=model.nodeData(id,QtNodes::NodeRole::Position).toPointF();
        out.positions[key]=QJsonObject{{"x",point.x()},{"y",point.y()}};
    }
    if(adapter)out.node_adapters[key]=node->adapter();
}
}

void CoreApplication::_clear_save_changes() {
    _save_graph_all=false;_save_nodes.clear();_save_positions.clear();_save_presentations.clear();_save_adapters.clear();
}
void CoreApplication::_invalidate_save_graph() {
    _save_graph_all=true;
    for(auto id:_ctx.data_flow_graph->allNodeIds())_bind_node_save_events(id);
}
void CoreApplication::_bind_node_save_events(unsigned int id) {
    auto* node=_ctx.data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(!node || node->property("saveEventsBound").toBool())return;
    node->setProperty("saveEventsBound",true);
    const auto processing=[this,id]{_save_nodes.insert(id);};
    connect(node,&AudioNodeModel::parameter_values_changed,this,processing);
    connect(node,&AudioNodeModel::parameter_structure_changed,this,processing);
    connect(node,&AudioNodeModel::presentation_changed,this,[this,id]{_save_presentations.insert(id);});
    connect(node,&AudioNodeModel::adapter_changed,this,[this,id]{_save_adapters.insert(id);});
}
void CoreApplication::_bind_incremental_saves() {
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());if(!model)return;
    _invalidate_save_graph();
    connect(model,&QtNodes::DataFlowGraphModel::nodePositionUpdated,this,[this](unsigned int id){_save_positions.insert(id);});
    connect(model,&QtNodes::DataFlowGraphModel::nodeCreated,this,[this](unsigned int id){_save_graph_all=true;_bind_node_save_events(id);});
    connect(model,&QtNodes::DataFlowGraphModel::nodeDeleted,this,[this]{_save_graph_all=true;});
    connect(model,&QtNodes::DataFlowGraphModel::connectionCreated,this,[this]{_save_graph_all=true;});
    connect(model,&QtNodes::DataFlowGraphModel::connectionDeleted,this,[this]{_save_graph_all=true;});
    connect(model,&NumericEditorGraphModel::persistence_topology_changed,this,[this]{_save_graph_all=true;});
    connect(_main_window->data_flow_editor(),&SchemeEditorWidget::execution_settings_changed,this,&CoreApplication::_invalidate_save_graph);
}
void CoreApplication::_capture_autosave_presentation(AsyncSave& job,NumericEditorGraphModel& model) {
    auto ids=_save_positions|_save_presentations|_save_adapters;
    if(job.full_graph)for(auto id:model.allNodeIds())ids.insert(id);
    for(auto id:std::as_const(ids)) {
        capture_placement(job.presentation,model,id,job.full_graph || _save_positions.contains(id),job.full_graph || _save_adapters.contains(id));
        if(job.full_graph || _save_presentations.contains(id))
            if(auto* node=model.delegateModel<AudioNodeModel>(id))job.presentation.node_presentations[QString::number(id)]=_persisted_node_presentation(node->presentation());
    }
    job.presentation.editor_view=_ctx.file_configs.ui.editor_view;
}
