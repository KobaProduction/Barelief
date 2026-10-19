#include <core_app/core_application.h>
#include <core_app/main_window.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <config/config_document.h>
#include <visualization/ui_text.h>
#include <QJsonArray>
#include <QFileInfo>
#include <QMessageBox>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QStatusBar>
#include <utility>

#include "core_application_state.h"
void CoreApplication::_update_configuration_save_status() {
    if(!_main_window)return;
    using State=MainWindow::ConfigurationSaveState;
    const auto state=!_configuration_load_error.isEmpty()?State::LoadError:!_configuration_save_error.isEmpty()?State::Error:_saving_configuration?State::Saving:(_configuration_project_dirty || _configuration_preferences_dirty || _ctx.editor_draft_modified)?State::Unsaved:State::Saved;
    _main_window->set_configuration_save_state(state,_config_manager.getConfigPath(),_configuration_error());
}
QString CoreApplication::_configuration_error() const {
    if(_configuration_load_error.isEmpty())return _configuration_save_error;
    const bool extra=!_configuration_save_error.isEmpty() && _configuration_save_error!=_configuration_load_error;
    return _configuration_load_error+(extra?"\n"+_configuration_save_error:QString{});
}
bool CoreApplication::_save_ui_preferences(bool force) {
    _configuration_preferences_dirty=true;_update_configuration_save_status();
    if(!_main_window || _saving_configuration)return false;
    if(!force && !_ctx.file_configs.ui.autosave_enabled)return true;
    _configuration_save_error.clear();
    QString error;
    const bool recovery=!_configuration_load_error.isEmpty();
    const bool saved=recovery?_config_manager.saveWorkspaceRecovery(_ctx.file_configs,&error):_persist_ui_preferences(error);
    return _finish_preferences_save(saved,error,recovery);
}
bool CoreApplication::_finish_preferences_save(bool saved,const QString& error,bool recovery) {
    if(saved)_configuration_preferences_dirty=false;
    else _configuration_save_error=error;
    _update_configuration_save_status();
    if(!saved && !recovery)_show_save_failure(_config_manager.getConfigPath(),error);
    return saved;
}
bool CoreApplication::_persist_ui_preferences(QString& error) {
    if(!_numeric_document){error=QStringLiteral("Canonical project document is unavailable");return false;}
    const QScopedValueRollback saving(_saving_configuration,true);
    _update_configuration_save_status();
    return _config_manager.saveNumericPreferences(*_numeric_document,_ctx.file_configs,&error);
}
void CoreApplication::_show_save_failure(const QString& path,const QString& error) {
    _main_window->statusBar()->showMessage(ui::text("Could not save settings: %1\n%2").arg(path,error),10000);
}
bool CoreApplication::_report_save_failure(const QString& error,const QString& path) {
    _configuration_save_error=error.isEmpty()?QStringLiteral("Configuration could not be saved"):error;
    _main_window->set_configuration_save_state(_configuration_load_error.isEmpty()?MainWindow::ConfigurationSaveState::Error:MainWindow::ConfigurationSaveState::LoadError,path,_configuration_save_error);
    _show_save_failure(path,error);return false;
}
bool CoreApplication::_save_configuration(bool force,const QString& saveAsPath) {
    if(!_main_window)return false;
    _configuration_project_dirty=true;_update_configuration_save_status();
    if(_saving_configuration)return true;
    if(!force && !_ctx.file_configs.ui.autosave_enabled)return true;
    _configuration_save_error.clear();
    const QScopedValueRollback saving(_saving_configuration,true);
    _update_configuration_save_status();
    return _save_current_project(force,saveAsPath);
}
bool CoreApplication::_save_current_project(bool force,const QString& saveAsPath) {
    SaveState state;
    state.path=saveAsPath.isEmpty()?_config_manager.getConfigPath():QFileInfo(saveAsPath).absoluteFilePath();
    state.target=desktop::numeric_project_path(state.path);
    state.recover_source=!_configuration_load_error.isEmpty();
    if(state.recover_source && !force)return _report_save_failure(ui::text("This project could not be opened. Select a current preset and use Save As to create a new project."),state.path);
    return _save_project(state,force);
}
bool CoreApplication::_save_project(SaveState& state,bool force) {
    if(!_prepare_save_destination(state,force))return false;
    if(!_collect_save_draft(state))return false;
    if(!_prepare_save_document(state))return false;
    auto& document=state.new_document?*state.new_document:*_numeric_document;
    QString error;
    if(!_collect_save_presentation(document,state.delta,error))return _report_save_failure(error,state.path);
    return _write_saved_project(state,document);
}
bool CoreApplication::_write_saved_project(SaveState& state,desktop::NumericProjectDocument& document) {
    desktop::NumericProjectDocument staged;
    auto result=desktop::stage_numeric_draft(document,state.draft,staged);
    if(result)result=desktop::stage_numeric_presentation(staged,state.delta,staged);
    if(result)result=desktop::save_numeric_project(staged,state.target,state.consent?&*state.consent:nullptr);
    if(!result)return _report_save_failure(result.error,state.path);
    document=std::move(staged);_commit_saved_project(state);return true;
}
bool CoreApplication::_prepare_save_destination(SaveState& state,bool force) {
    if(!force)return true;
    desktop::NumericSaveConsent destination;
    const auto inspected=desktop::inspect_numeric_save_destination(state.target,destination);
    if(!inspected)return _report_save_failure(inspected.error,state.path);
    const bool unsupported=destination.existed && !_readable_save_destination(destination);
    // Exact observed bytes authorize recreation or confirmed replacement only.
    if(!destination.existed || unsupported)state.consent=std::move(destination);
    if(!unsupported || _confirm_configuration_overwrite(state.target))return true;
    _main_window->set_configuration_save_state(state.recover_source?MainWindow::ConfigurationSaveState::LoadError:MainWindow::ConfigurationSaveState::Unsaved,_config_manager.getConfigPath(),_configuration_load_error);
    return false;
}
bool CoreApplication::_readable_save_destination(const desktop::NumericSaveConsent& destination) const {
    desktop::NumericProjectDocument readable;domain::NumericProcessingGraph loaded;
    return desktop::read_numeric_project(destination.bytes,readable) && desktop::read_numeric_draft(readable,loaded);
}
bool CoreApplication::_confirm_configuration_overwrite(const QString& target) {
    QMessageBox dialog(QMessageBox::Warning,ui::text("Overwrite configuration?"),ui::text("Replace the contents of %1 with the current configuration? This cannot be undone.").arg(target),QMessageBox::NoButton,_main_window);
    dialog.setObjectName("confirmConfigurationOverwrite");
    dialog.setOption(QMessageBox::Option::DontUseNativeDialog);dialog.setTextFormat(Qt::PlainText);
    auto* overwrite=dialog.addButton(ui::text("Overwrite"),QMessageBox::AcceptRole);
    auto* cancel=dialog.addButton(QMessageBox::Cancel);cancel->setText(ui::text("Cancel"));
    dialog.setDefaultButton(cancel);dialog.setEscapeButton(cancel);dialog.exec();
    return dialog.clickedButton()==overwrite;
}
bool CoreApplication::_collect_save_draft(SaveState& state) {
    auto* model=dynamic_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
    if(!model || (!_numeric_draft_loaded && !model->has_numeric_nodes()))return _report_save_failure(QStringLiteral("Canonical numeric editor is unavailable"),state.path);
    auto* editor=_main_window->data_flow_editor();
    if(!editor)return _report_save_failure(ui::text("Numeric editor unavailable"),state.path);
    QString error;
    if(!editor->numeric_draft(state.draft,&error,false))return _report_save_failure(error,state.path);
    editor->flush_presentation();return true;
}
bool CoreApplication::_prepare_save_document(SaveState& state) {
    if(_numeric_document && !state.recover_source)return true;
    if(state.recover_source && state.draft.nodes.empty())return _report_save_failure(ui::text("This project could not be opened. Select a current preset and use Save As to create a new project."),state.path);
    state.new_document=std::make_unique<desktop::NumericProjectDocument>();
    const auto result=desktop::create_numeric_project(state.draft,{},*state.new_document);
    return result?true:_report_save_failure(result.error,state.path);
}
bool CoreApplication::_collect_save_presentation(const desktop::NumericProjectDocument& document,desktop::NumericDraftPresentation& delta,QString& error) {
    desktop::NumericDraftPresentation before;
    const auto read=desktop::read_numeric_presentation(document,before);
    if(!read){error=read.error;return false;}
    delta.applied_graph=_presenter->applied_numeric_graph();
    auto* model=static_cast<NumericEditorGraphModel*>(_ctx.data_flow_graph.get());
    for(auto id:model->allNodeIds())_collect_node_presentation(*model,id,before,delta);
    delta.editor_view=configuration::changed_fields(before.editor_view,_ctx.file_configs.ui.editor_view);
    return _config_manager.numericSettingsDelta(document,_ctx.file_configs,delta.application_settings,&error);
}
void CoreApplication::_collect_node_presentation(NumericEditorGraphModel& model,unsigned int id,const desktop::NumericDraftPresentation& before,desktop::NumericDraftPresentation& delta) const {
    const auto key=QString::number(id);
    const auto point=model.nodeData(id,QtNodes::NodeRole::Position).toPointF();
    const auto changed=configuration::changed_fields(before.positions.value(key).toObject(),QJsonObject{{"x",point.x()},{"y",point.y()}});
    if(!changed.isEmpty())delta.positions[key]=changed;
    auto* node=model.delegateModel<AudioNodeModel>(id);if(!node)return;
    if(before.node_adapters.value(key).toObject()!=node->adapter())delta.node_adapters[key]=node->adapter();
    const auto changed_presentation=configuration::changed_fields(before.node_presentations.value(key).toObject(),_persisted_node_presentation(node->presentation()));
    if(!changed_presentation.isEmpty())delta.node_presentations[key]=changed_presentation;
}
QJsonObject CoreApplication::_persisted_node_presentation(const QJsonObject& presentation) const {
    return {{"inline_parameters",presentation.value("inline_parameters").toArray()},
        {"input_port_order",presentation.value("input_port_order").toArray()},
        {"output_port_order",presentation.value("output_port_order").toArray()},
        {"port_labels",presentation.value("port_labels").toObject()},
        {"show_dc_outputs",presentation.value("show_dc_outputs").toBool()},
        {"display_name",presentation.value("display_name").toString()},
        {"scalar_inputs",presentation.value("scalar_inputs").toObject()},
        {"stream_names",presentation.value("stream_names").toArray()}};
}
void CoreApplication::_commit_saved_project(SaveState& state) {
    auto& document=state.new_document?*state.new_document:*_numeric_document;
    _config_manager.setConfigPath(document.active_path);
    if(state.new_document)_numeric_document=std::move(state.new_document);
    if(state.recover_source){_configuration_load_error.clear();_presenter->clear_file_protection_warning();}
    _numeric_draft_loaded=true;_ctx.editor_draft_modified=false;
    _configuration_project_dirty=false;_configuration_preferences_dirty=false;
    _main_window->set_configuration_save_state(MainWindow::ConfigurationSaveState::Saved,_config_manager.getConfigPath());
}
