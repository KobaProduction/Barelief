#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/numeric_node_operations.h"
#include <desktop/numeric_project_document.h>
#include <QJsonDocument>
#include <QJsonArray>
#include <barelief/ui_text.h>
#include <algorithm>
#include <utility>
#include "core/performance.h"

namespace {
QString engine_diagnostic_text(const pipeline::EngineDiagnostic& value) {
    const auto parameter=value.parameter.toUtf8();
    domain::ProcessingDiagnostic diagnostic{value.error,value.node,value.port,value.edge,value.reason,
        std::string_view(parameter.constData(),std::size_t(parameter.size()))};
    return numeric_editor::processing_diagnostic_text(diagnostic);
}
QString engine_reply_error(const pipeline::EngineReply& reply) {
    return reply.numeric_diagnostic?engine_diagnostic_text(*reply.numeric_diagnostic):reply.error;
}
QMap<int,QJsonObject> node_adapter_map(const QJsonArray& nodes) {
    QMap<int,QJsonObject> adapters;
    for(const auto& value:nodes) {
        const auto node=value.toObject();
        adapters.insert(node.value("id").toInt(),node.value("adapter").toObject());
    }
    return adapters;
}
}
GuiPresenter::GuiPresenter(GuiState& state,std::shared_ptr<pipeline::EngineProvider> provider,
    const QString& host,quint16 port,QObject* parent):QObject(parent),_remote(!host.isEmpty()),
    _state(state),_provider(std::move(provider)) {
    _device_id=_remote?host+":"+QString::number(port):_local_device_id;
    if(_remote)_remote_endpoint={{"address",host},{"port",port}};
    _client=_provider->create(host,port);bind_client();
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this] {
        if(!_last_numeric_rejection)return;
        _apply_status=ui::text("Not applied: ")+engine_diagnostic_text(*_last_numeric_rejection);
        emit apply_status_changed(_apply_status);
    });
}
GuiPresenter::~GuiPresenter()=default;
void GuiPresenter::set_numeric_preview_cadence_hz(int hz) {
    const int normalized=std::clamp(hz,1,240);
    if(_numeric_preview_cadence_hz==normalized)return;
    _numeric_preview_cadence_hz=normalized;
    _client->set_numeric_preview_cadence_hz(_numeric_preview_cadence_hz);
}
void GuiPresenter::bind_client() {
    _client->set_numeric_preview_cadence_hz(_numeric_preview_cadence_hz);
    _accepted_service_epoch.reset();
    _published_component_statuses.clear();
    const auto selected=_device_id;
    const auto generation=++_binding_generation;
    bind_control_handlers(selected,generation);
    bind_numeric_handlers(selected,generation);
}
void GuiPresenter::bind_control_handlers(const QString& selected,quint64 generation) {
    _client->set_snapshot_handler([this,selected,generation](const auto& snapshot){if(selected==_device_id && generation==_binding_generation)accept_snapshot(snapshot);});
    _client->set_reply_handler([this,selected,generation](const auto& reply) {
        if(selected!=_device_id || generation!=_binding_generation)return;
        accept_graph_reply(reply);
    });
}
void GuiPresenter::accept_graph_reply(const pipeline::EngineReply& reply) {
    const auto error=engine_reply_error(reply);
    if(!_pending_graphs.contains(reply.request_id)) {
        if(!reply.accepted)emit command_rejected(reply.request_id,error);
        return;
    }
    auto pending=_pending_graphs.take(reply.request_id);
    set_apply_status(reply.accepted?ui::text("Applied"):ui::text("Not applied: ")+error);
    if(!reply.accepted)_last_numeric_rejection=reply.numeric_diagnostic;
    if(reply.accepted) {
        accept_applied_graph(reply);
        emit on_config_updated();
    }
    emit graph_apply_finished(pending.logical_id,reply.accepted,error);
    dispatch_queued_graph();
}
void GuiPresenter::dispatch_queued_graph() {
    if(_queued_graph){auto next=std::move(*_queued_graph);_queued_graph.reset();dispatch_graph(std::move(next));}
}
void GuiPresenter::accept_applied_graph(const pipeline::EngineReply& reply) {
        domain::NumericProcessingGraph applied;
        if(desktop::decode_numeric_graph(QJsonDocument(reply.snapshot.graph).toJson(QJsonDocument::Compact),applied))
            _applied_numeric=std::move(applied);
}
void GuiPresenter::bind_numeric_handlers(const QString& selected,quint64 generation) {
    bind_numeric_frame_handler(selected,generation);
    bind_numeric_history_handler(selected,generation);
    bind_numeric_snapshot_handler(selected,generation);
}
void GuiPresenter::bind_numeric_frame_handler(const QString& selected,quint64 generation) {
    _client->set_numeric_frame_handler([this,selected,generation](auto frame) {
        if(selected!=_device_id || generation!=_binding_generation)return;
        const auto snapshot=_client->snapshot();
        if(!_accepted_service_epoch || *_accepted_service_epoch!=snapshot.epoch)accept_snapshot(snapshot);
        if(frame.delivery.epoch==snapshot.epoch && frame.delivery.graph_revision==snapshot.graph_revision)
            emit numeric_frame_ready(frame);
    });
}
void GuiPresenter::bind_numeric_history_handler(const QString& selected,quint64 generation) {
    _client->set_numeric_history_handler([this,selected,generation](auto row) {
        if(selected!=_device_id || generation!=_binding_generation)return;
        const auto snapshot=_client->snapshot();
        if(!_accepted_service_epoch || *_accepted_service_epoch!=snapshot.epoch)accept_snapshot(snapshot);
        if(row.compact.delivery.epoch==snapshot.epoch && row.compact.delivery.graph_revision==snapshot.graph_revision)
            emit numeric_history_ready(row);
    });
}
void GuiPresenter::bind_numeric_snapshot_handler(const QString& selected,quint64 generation) {
    _client->set_numeric_history_snapshot_handler([this,selected,generation](auto frame,std::uint64_t ordinal,std::uint64_t filled_count) {
        if(selected!=_device_id || generation!=_binding_generation)return;
        const auto snapshot=_client->snapshot();
        if(!_accepted_service_epoch || *_accepted_service_epoch!=snapshot.epoch)accept_snapshot(snapshot);
        if(frame.delivery.epoch==snapshot.epoch && frame.delivery.graph_revision==snapshot.graph_revision)
            emit numeric_history_snapshot_ready(frame,ordinal,filled_count);
    });
}
void GuiPresenter::set_apply_status(QString status){_last_numeric_rejection.reset();_apply_status=std::move(status);emit apply_status_changed(_apply_status);}
QString GuiPresenter::visible_saved_warning() const{return _file_protection_warning.isEmpty()?_saved_graph_warning:_file_protection_warning;}
void GuiPresenter::clear_file_protection_warning() {
    if(_file_protection_warning.isEmpty())return;
    _file_protection_warning.clear();_saved_graph_warning.clear();
    emit saved_graph_warning_changed({});
}
void GuiPresenter::initialize(){initialize(_state.file_configs.raw_document);}
void GuiPresenter::initialize(QJsonObject config,const QString& error) {
    if(!error.isEmpty()) {_file_protection_warning=error;_saved_graph_warning=error;
        emit saved_graph_warning_changed(error);set_apply_status(ui::text("Not applied: ")+error);return;}
    QString network_error;
    if(!_client->configure_network(config,&network_error)){_saved_graph_warning=network_error;emit saved_graph_warning_changed(network_error);}
    if(_remote)_client->connect_remote();
    set_apply_status(ui::text("Draft"));
    // The composition root starts an admitted project after all document and
    // adapter boundaries have loaded successfully. This snapshot is read-only.
    pipeline::EngineCommand command;command.kind=pipeline::EngineCommand::Snapshot;_client->submit(command);
}
void GuiPresenter::accept_snapshot(const pipeline::EngineSnapshot& snapshot) {
    core::perf::Scope profile(core::perf::Stage::PresenterSnapshot);
    const bool incarnation_changed=!_accepted_service_epoch || *_accepted_service_epoch!=snapshot.epoch;
    _accepted_service_epoch=snapshot.epoch;
    if(incarnation_changed)emit views_cleared();
    if(incarnation_changed || snapshot.graph!=_active_graph) {
        _active_graph=snapshot.graph;
        domain::NumericProcessingGraph graph;
        if(!snapshot.graph.isEmpty() && desktop::decode_numeric_graph(QJsonDocument(snapshot.graph).toJson(QJsonDocument::Compact),graph)) {
            _graph_id=QString::fromStdString(graph.graph_id);_applied_numeric=std::move(graph);
        } else {
            _applied_numeric.reset();_graph_id="main";
        }
        emit active_graph_changed(snapshot.graph);update_subscription();
    }
    // Status is control-plane state, not a notification for each data delivery.
    // Refresh diagnostics below independently; a new service incarnation must
    // still publish its initial state even when its values match the old one.
    publish_component_statuses(snapshot,incarnation_changed);
    emit processing_state_changed();
}
void GuiPresenter::publish_component_statuses(const pipeline::EngineSnapshot& snapshot,bool incarnation_changed) {
    const auto previous=std::move(_published_component_statuses);
    _published_component_statuses=snapshot.statuses;
    for(auto it=snapshot.statuses.begin();it!=snapshot.statuses.end();++it) {
        const auto prior=previous.constFind(it.key());
        if(incarnation_changed || prior==previous.cend() || prior->state!=it->state || prior->error!=it->error)
            emit component_status_changed(it.key(),int(it->state),it->error);
    }
}
quint64 GuiPresenter::dispatch_graph(PendingGraph request) {
    pipeline::EngineCommand command;command.kind=pipeline::EngineCommand::ApplyGraph;
    command.graph=request.document;command.expected_revision=_client->snapshot().graph_revision;
    const auto id=_client->submit(command);
    if(!id){const auto error=QStringLiteral("Desktop command queue exhausted");set_apply_status(ui::text("Not applied: ")+error);emit graph_apply_finished(request.logical_id,false,error);return 0;}
    _pending_graphs[id]=std::move(request);set_apply_status(ui::text("Applying"));return id;
}
quint64 GuiPresenter::submit_graph(PendingGraph request) {
    request.logical_id=++_logical_request;
    const auto logical=request.logical_id;
    if(!_pending_graphs.isEmpty()) {
        if(_queued_graph)emit graph_apply_finished(_queued_graph->logical_id,false,QStringLiteral("Superseded draft"));
        _queued_graph=std::move(request);
    } else dispatch_graph(std::move(request));
    return logical;
}
QJsonObject GuiPresenter::graph_document_with_adapters(QJsonObject document) const {
    if(!_graph_adapter_provider)return document;
    const auto metadata=_graph_adapter_provider();
    if(metadata.value("graph_id")!=document.value("graph_id"))return document;
    auto nodes=document.value("nodes").toArray();
    const auto adapters=node_adapter_map(metadata.value("nodes").toArray());
    for(qsizetype i=0;i<nodes.size();++i) {
        auto node=nodes[i].toObject();
        if(adapters.contains(node.value("id").toInt()))node["adapter"]=adapters.value(node.value("id").toInt());
        nodes[i]=node;
    }
    document["nodes"]=nodes;
    return document;
}
quint64 GuiPresenter::apply_pipeline_graph(const QJsonObject&) {
    domain::NumericProcessingGraph graph;QString error;
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_state.data_flow_graph.get());
    if(!model || !model->export_numeric_graph(graph,&error)){set_apply_status(ui::text("Not applied: ")+error);return 0;}
    QByteArray bytes;const auto encoded=desktop::encode_numeric_graph(graph,bytes);
    if(!encoded){set_apply_status(encoded.error);return 0;}
    PendingGraph pending;pending.document=graph_document_with_adapters(QJsonDocument::fromJson(bytes).object());return submit_graph(std::move(pending));
}
quint64 GuiPresenter::set_node_parameters(int,const QJsonObject&) {
    // Processing edits belong to registered typed fields in the graph editor.
    set_apply_status(ui::text("Draft"));return 0;
}
void GuiPresenter::set_spectrum_channel(int channel){_state.file_configs.core.spectrum_channel=channel;emit on_config_updated();}
void GuiPresenter::set_spectrum_merge(int){set_apply_status(ui::text("Draft"));}
void GuiPresenter::_sync_processing_config(){}
void GuiPresenter::apply_processing_settings(){apply_pipeline_graph({});}
void GuiPresenter::start_wled_audio_reactive_server(){set_apply_status(ui::text("Draft"));}
void GuiPresenter::stop_wled_audio_reactive_server(){}
void GuiPresenter::restart_wled_audio_reactive_server(){start_wled_audio_reactive_server();}
void GuiPresenter::set_visible_views(quint32 mask){_visible_views=mask;}
void GuiPresenter::update_subscription() {
    auto endpoints=_numeric_endpoints;
    for(const auto& endpoint:_editor_numeric_endpoints)if(std::find(endpoints.begin(),endpoints.end(),endpoint)==endpoints.end())endpoints.push_back(endpoint);
    QString error;if(!_client->subscribe_numeric_frames(std::move(endpoints),&error)){_saved_graph_warning=error;emit saved_graph_warning_changed(error);}
}
void GuiPresenter::set_editor_numeric_endpoints(std::vector<pipeline::EndpointIdentity> endpoints) {
    _editor_numeric_endpoints=std::move(endpoints);update_subscription();
}
void GuiPresenter::set_numeric_endpoints(std::vector<pipeline::EndpointIdentity> endpoints) {
    _numeric_endpoints=std::move(endpoints);update_subscription();
}
void GuiPresenter::save_device_document() {
    auto devices=_state.file_configs.raw_document.value("devices").toObject();
    auto device=devices.value(_device_id).toObject();
    device["scene_graph"]=_state.file_configs.scene_graph;
    device["presets"]=_state.file_configs.ui.pipeline_presets;
    device["editor_view"]=_state.file_configs.ui.editor_view;
    devices[_device_id]=device;
    _state.file_configs.raw_document["devices"]=devices;
}
void GuiPresenter::select_device(const QString& uuid,const QString& host,quint16 port) {
    if(uuid.isEmpty() || uuid==_device_id)return;
    save_device_document();
    release_selected_client();
    switch_selected_client(uuid,host,port);
    bind_client();_active_graph={};_applied_numeric.reset();_graph_id="main";emit views_cleared();
    restore_selected_device_document();
    if(_remote)_client->connect_remote();accept_snapshot(_client->snapshot());
}
void GuiPresenter::release_selected_client() {
    const auto pending_graphs=_pending_graphs;
    for(const auto& pending:pending_graphs)emit graph_apply_finished(pending.logical_id,false,QStringLiteral("Selected device changed"));
    _pending_graphs.clear();_queued_graph.reset();_numeric_endpoints.clear();_editor_numeric_endpoints.clear();
    _client->set_numeric_frame_handler({});_client->set_numeric_history_handler({});_client->set_numeric_history_snapshot_handler({});
    _client->set_snapshot_handler({});_client->set_reply_handler({});
}
void GuiPresenter::switch_selected_client(const QString& uuid,const QString& host,quint16 port) {
    _inactive_clients[_device_id]=std::move(_client);
    _device_id=uuid;_remote=uuid!=_local_device_id;
    _remote_endpoint=_remote?QJsonObject{{"address",host},{"port",port}}:QJsonObject{};
    auto found=_inactive_clients.find(uuid);
    if(found!=_inactive_clients.end()){_client=std::move(found->second);_inactive_clients.erase(found);}
    else _client=_provider->create(_remote?host:QString{},port);
}
void GuiPresenter::restore_selected_device_document() {
    const auto saved=_state.file_configs.raw_document["devices"].toObject()[_device_id].toObject();
    _state.file_configs.ui.pipeline_presets=saved.value("presets").toArray();
    if(!saved.isEmpty())emit selected_device_changed(saved["scene_graph"].toObject(),saved["editor_view"].toObject());
}
