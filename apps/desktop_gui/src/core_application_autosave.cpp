#include <core/diagnostic_saves.h>
#include "core_application_state.h"
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <config/config_document.h>
#include <core/performance.h>
#include <QElapsedTimer>
#include <QDebug>
#include <QTimer>
#include <exception>
#include <utility>
namespace {
void retain_changed_fields(QJsonObject& updates,const QJsonObject& before) {
    for(auto it=updates.begin();it!=updates.end();) {
        const auto changed=configuration::changed_fields(before.value(it.key()).toObject(),it.value().toObject());
        if(changed.isEmpty())it=updates.erase(it);
        else {it.value()=changed;++it;}
    }
}
void exclude_editor_view(QJsonObject& settings) {
    auto ui=settings.value("ui").toObject();ui.remove("editor_view");
    if(ui.isEmpty())settings.remove("ui");else settings["ui"]=ui;
}
void retain_changed_adapters(QJsonObject& updates,const QJsonObject& before) {
    for(auto it=updates.begin();it!=updates.end();)
        if(before.value(it.key()).toObject()==it.value().toObject())it=updates.erase(it);else ++it;
}
}

bool CoreApplication::AsyncSave::prepare_presentation() {
    desktop::NumericDraftPresentation before;
    auto result=desktop::read_numeric_presentation(*document,before);
    if(!result){error=result.error;return false;}
    retain_changed_fields(presentation.positions,before.positions);
    retain_changed_fields(presentation.node_presentations,before.node_presentations);
    retain_changed_adapters(presentation.node_adapters,before.node_adapters);
    presentation.editor_view=configuration::changed_fields(before.editor_view,presentation.editor_view);
    ConfigManager manager(path);
    if(!manager.numericSettingsDelta(*document,config,presentation.application_settings,&error))return false;
    // editor_view has its own validated delta; do not stage it a second time.
    exclude_editor_view(presentation.application_settings);return true;
}
void CoreApplication::AsyncSave::save_project() {
    if(!prepare_presentation())return;
    if(!full_graph && nodes.empty() && presentation.positions.isEmpty() && presentation.node_presentations.isEmpty()
        && presentation.node_adapters.isEmpty() && presentation.editor_view.isEmpty() && presentation.application_settings.isEmpty()
        && !presentation.applied_graph){saved=true;return;}
    desktop::NumericProjectDocument staged;
    auto result=full_graph?desktop::stage_numeric_draft(*document,draft,staged)
        :nodes.empty()?desktop::stage_numeric_presentation(*document,presentation,staged)
        :desktop::stage_numeric_nodes(*document,nodes,staged);
    if(result && (full_graph || !nodes.empty()))result=desktop::stage_numeric_presentation(staged,presentation,staged);
    if(result)result=desktop::save_numeric_project(staged,path);
    saved=bool(result);error=result.error;
    if(saved)*document=std::move(staged);
}
void CoreApplication::AsyncSave::run() {
    const core::perf::Scope trace(core::perf::Stage::ProjectSaveWrite);
    QElapsedTimer timer;timer.start();
    try {
        if(project)save_project();
        else {
            ConfigManager manager(path);
            saved=manager.saveNumericPreferences(*document,config,&error);
        }
    } catch(const std::exception& failure) { error=QString::fromUtf8(failure.what()); }
    if(core::diagnostics::trace_saves())
        qInfo().noquote()<<"BARELIEF_SAVE_TRACE background=completed success="<<saved<<" ms="<<timer.nsecsElapsed()/1e6;
}
void CoreApplication::_queue_autosave() {
    if(!_main_window || _saving_configuration || _autosave_queued || !_ctx.file_configs.ui.autosave_enabled)return;
    _autosave_queued=true;
    // Coalesce notifications from one event; this adds no inactivity timeout.
    QTimer::singleShot(0,this,[this]{if(_autosave_queued){_autosave_queued=false;_start_autosave();}});
}
bool CoreApplication::_capture_autosave_graph(AsyncSave& job) {
    auto* editor=_main_window->data_flow_editor();if(!editor)return false;
    editor->flush_presentation();job.full_graph=_save_graph_all;
    if(job.full_graph && !editor->numeric_draft(job.draft,&job.error,false))return _report_save_failure(job.error,_config_manager.getConfigPath());
    auto* model=static_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
    for(auto id:std::as_const(_save_nodes))if(auto* node=model->delegateModel<AudioNodeModel>(id);node && node->numeric_node())job.nodes.push_back(*node->numeric_node());
    _capture_autosave_presentation(job,*model);
    return true;
}
bool CoreApplication::_capture_autosave(AsyncSave& job) {
    const core::perf::Scope trace(core::perf::Stage::ProjectSaveCapture);
    if(job.project && !_capture_autosave_graph(job))return false;
    job.config=_ctx.file_configs;job.path=_config_manager.getConfigPath();
    job.project_revision=_project_save_revision;job.preferences_revision=_preferences_save_revision;
    const auto snapshot=_presenter->client().snapshot();job.applied_epoch=snapshot.epoch;job.applied_revision=snapshot.graph_revision;
    if(job.project) {
        if(job.applied_epoch!=_saved_applied_epoch || job.applied_revision!=_saved_applied_revision)
            job.presentation.applied_graph=_presenter->applied_numeric_graph();
        _clear_save_changes();
    }
    return true;
}
void CoreApplication::_start_autosave() {
    if(_saving_configuration || !_ctx.file_configs.ui.autosave_enabled || !_numeric_document || !_configuration_load_error.isEmpty())return;
    if(!_configuration_project_dirty && !_configuration_preferences_dirty)return;
    auto job=std::make_unique<AsyncSave>();job->project=_configuration_project_dirty;
    _saving_configuration=true;
    QElapsedTimer capture;capture.start();
    if(!_capture_autosave(*job)){_saving_configuration=false;_update_configuration_save_status();return;}
    if(core::diagnostics::trace_saves())qInfo().noquote()<<"BARELIEF_SAVE_PHASE gui_capture ms="<<capture.nsecsElapsed()/1e6
        <<" full_graph="<<job->full_graph<<" nodes="<<job->nodes.size()<<" positions="<<job->presentation.positions.size();
    _launch_autosave(std::move(job));
}
void CoreApplication::_launch_autosave(std::unique_ptr<AsyncSave> job) {
    if(!_save_thread) {
        _save_thread=std::make_unique<QThread>();_save_worker=new QObject;
        _save_worker->moveToThread(_save_thread.get());
        connect(_save_thread.get(),&QThread::finished,_save_worker,&QObject::deleteLater);
        _save_thread->start();
    }
    job->document=std::move(_numeric_document);
    _async_save=std::move(job);auto* pending=_async_save.get();
    const auto id=++_save_job_id;
    QMetaObject::invokeMethod(_save_worker,[this,pending,id]{
        pending->run();QMetaObject::invokeMethod(this,[this,id]{_complete_autosave(id);},Qt::QueuedConnection);
    },Qt::QueuedConnection);
    _update_configuration_save_status();
}
void CoreApplication::_complete_autosave(quint64 id) {
    if(id!=_save_job_id || !_async_save)return;
    _finish_autosave(false);
    if(_configuration_save_error.isEmpty() && (_configuration_project_dirty || _configuration_preferences_dirty))_queue_autosave();
}
void CoreApplication::_finish_autosave(bool wait) {
    if(!_async_save)return;
    if(wait)QMetaObject::invokeMethod(_save_worker,[]{},Qt::BlockingQueuedConnection);
    auto job=std::move(_async_save);
    _numeric_document=std::move(job->document);_saving_configuration=false;
    if(!job->saved){_save_graph_all=true;_report_save_failure(job->error,job->path);return;}
    if(job->project){_saved_applied_epoch=job->applied_epoch;_saved_applied_revision=job->applied_revision;}
    _accept_autosave(*job);
}
void CoreApplication::_accept_autosave(const AsyncSave& job) {
    _configuration_save_error.clear();
    if(job.project && job.project_revision==_project_save_revision) {
        _configuration_project_dirty=false;_ctx.editor_draft_modified=false;_numeric_draft_loaded=true;
    }
    if(job.preferences_revision==_preferences_save_revision)_configuration_preferences_dirty=false;
    _update_configuration_save_status();
}
void CoreApplication::_flush_autosave() {
    _autosave_queued=false;_finish_autosave();
    if(!_main_window || !_ctx.file_configs.ui.autosave_enabled || _saving_configuration)return;
    if(!_configuration_load_error.isEmpty()){if(_configuration_preferences_dirty)_save_ui_preferences(true);return;}
    if(_configuration_project_dirty)_save_configuration(true);
    else if(_configuration_preferences_dirty)_save_ui_preferences(true);
}
