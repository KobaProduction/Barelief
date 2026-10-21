

#include "core_app/config_manager.h"


#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
#include <QSettings>
#include <QFileInfo>
#include <QCryptographicHash>
#include <utility>

namespace {
QJsonObject changed_settings_fields(const QJsonObject& before,const QJsonObject& after) {
    QJsonObject result;
    for(auto it=after.begin();it!=after.end();++it){
        if(before.value(it.key())==it.value())continue;
        result[it.key()]=before.value(it.key()).isObject()&&it.value().isObject()
            ?QJsonValue(changed_settings_fields(before.value(it.key()).toObject(),it.value().toObject())):it.value();
    }
    return result;
}
}





ConfigManager::ConfigManager(QString config_path) : _config_path(desktop::numeric_project_path(config_path)) {}

bool ConfigManager::loadNumeric(desktop::NumericProjectDocument& document,domain::NumericProcessingGraph& graph,QString* error) const {
    const auto result=desktop::load_numeric_project(_config_path,document,graph);
    if(error)*error=result.path.isEmpty()?result.error:result.path+": "+result.error;
    return static_cast<bool>(result);
}

bool ConfigManager::readNumericSettings(const desktop::NumericProjectDocument& document,FileSaveApplicationConfig& config,QString* error) const {
    QJsonObject projection;const auto result=desktop::read_numeric_application_settings(document,projection);
    if(error)*error=result.path.isEmpty()?result.error:result.path+": "+result.error;
    if(!result)return false;
    FileSaveApplicationConfig candidate;_parse_core_config(projection,candidate);_parse_ui_config(projection,candidate);_parse_wled_ar_server_config(projection,candidate);
    config=std::move(candidate);return true;
}

bool ConfigManager::numericSettingsDelta(const desktop::NumericProjectDocument& document,const FileSaveApplicationConfig& config,QJsonObject& out,QString* error) const {
    FileSaveApplicationConfig before;if(!readNumericSettings(document,before,error))return false;
    QJsonObject baseline,current;_save_aggregator_config(baseline,before);_save_ui_config(baseline,before);_save_wled_ar_server_config(baseline,before);
    _save_aggregator_config(current,config);_save_ui_config(current,config);_save_wled_ar_server_config(current,config);
    out=changed_settings_fields(baseline,current);return true;
}

bool ConfigManager::saveNumericPreferences(desktop::NumericProjectDocument& document,
                                         const FileSaveApplicationConfig& config, QString* error) const {
    QJsonObject settings;
    if (!numericSettingsDelta(document, config, settings, error)) return false;
    desktop::NumericDraftPresentation delta;
    auto preferences = settings.value("ui").toObject();
    // A source may omit this preference and inherit true. Re-enabling autosave
    // after an in-memory disable still expresses an explicit persistence choice.
    preferences["autosave_enabled"] = config.ui.autosave_enabled;
    delta.application_settings["ui"] = preferences;
    desktop::NumericProjectDocument candidate;
    auto result = desktop::stage_numeric_presentation(document, delta, candidate);
    if (result) result = desktop::save_numeric_project(candidate, _config_path);
    if (error) *error = result.path.isEmpty() ? result.error : result.path + ": " + result.error;
    if (result) document = std::move(candidate);
    return static_cast<bool>(result);
}

namespace {
constexpr qsizetype workspace_recovery_limit=4*1024*1024;
QByteArray project_identity(const QString& path){QFile file(path);if(!file.open(QIODevice::ReadOnly) || file.size()>16777216)return {};return QCryptographicHash::hash(file.readAll(),QCryptographicHash::Sha256);}

bool recovery_matches_project(QSettings& settings,const QString& project_path) {
    return settings.status()==QSettings::NoError && settings.value("version").toInt()==1
        && settings.value("project_sha256").toByteArray()==project_identity(project_path);
}

QJsonObject recovery_presentation(const QJsonObject& ui) {
    QJsonObject presentation{{"version",1}};
    for(const auto* key:{"workspace","graph_views","graph_rendering","show_raw_samples","show_amplitudes","show_test_amplitudes","show_spectrogram","show_window_signal","show_windowed_signal","show_window_function","show_data_flow"}){
        if(ui.contains(key))presentation[key]=ui.value(key);
    }
    return presentation;
}

bool recovery_file_is_writable(QSettings& settings,const QString& path,QString* error) {
    if(settings.status()==QSettings::NoError && (!QFileInfo::exists(path) || settings.value("version").toInt()==1))return true;
    if(error)*error="Unsupported workspace recovery file";
    return false;
}

bool recovery_failure(QString* error,const QString& message) {
    if(error)*error=message;
    return false;
}
bool validate_recovery_presentation(const QByteArray& bytes,QJsonObject& checked,QString* error) {
    const auto result=desktop::decode_numeric_preset_presentation(bytes,checked);
    if(!result || bytes.size()>workspace_recovery_limit)
        return recovery_failure(error,result.error.isEmpty()?QStringLiteral("Workspace recovery limit"):result.error);
    return true;
}
bool read_recovery_presentation(QSettings& settings,const QString& project_path,QJsonObject& checked,QString* error) {
    if(!recovery_matches_project(settings,project_path))return recovery_failure(error,"Workspace recovery is incompatible with the project");
    const auto result=desktop::decode_numeric_preset_presentation(settings.value("presentation").toByteArray(),checked);
    return result?true:recovery_failure(error,result.error);
}
bool write_recovery_presentation(QSettings& settings,const QString& project_path,const QString& path,const QByteArray& bytes,QString* error) {
    settings.setValue("version",1);settings.setValue("project_sha256",project_identity(project_path));settings.setValue("presentation",bytes);settings.sync();
    if(settings.status()!=QSettings::NoError)return recovery_failure(error,"Could not save workspace recovery: "+path);
    return true;
}
desktop::NumericDocumentResult stage_numeric_candidate(
    const desktop::NumericProjectDocument& document,
    const domain::NumericProcessingGraph& graph,
    const desktop::NumericDraftPresentation& presentation,
    desktop::NumericProjectDocument& candidate) {
    auto result=desktop::stage_numeric_draft(document,graph,candidate);
    const bool has_presentation=!presentation.positions.isEmpty()
        || !presentation.node_presentations.isEmpty()
        || !presentation.node_adapters.isEmpty()
        || !presentation.editor_view.isEmpty()
        || !presentation.application_settings.isEmpty()
        || presentation.applied_graph;
    if(result && has_presentation){
        result=desktop::stage_numeric_presentation(candidate,presentation,candidate);
    }
    return result;
}
}
bool ConfigManager::loadWorkspaceRecovery(FileSaveApplicationConfig& config,QString* error) const {
    const auto path=_config_path+".workspace.ini";
    if(!QFileInfo::exists(path))return false;
    if(QFileInfo(path).size()>workspace_recovery_limit)return recovery_failure(error,"Workspace recovery limit");
    QSettings settings(path,QSettings::IniFormat);settings.setFallbacksEnabled(false);settings.sync();
    QJsonObject checked;
    if(!read_recovery_presentation(settings,_config_path,checked,error))return false;
    merge_recovery_presentation(config,checked);return true;
}
void ConfigManager::merge_recovery_presentation(FileSaveApplicationConfig& config,const QJsonObject& checked) {
    QJsonObject existing;_save_ui_config(existing,config);auto ui=existing.value("ui").toObject();
    for(auto it=checked.begin();it!=checked.end();++it)if(it.key()!="version")ui[it.key()]=it.value();
    auto candidate=config;_parse_ui_config(QJsonObject{{"ui",ui}},candidate);config=std::move(candidate);
}
QByteArray ConfigManager::encode_recovery_presentation(const FileSaveApplicationConfig& config) {
    QJsonObject current;_save_ui_config(current,config);
    const auto presentation=recovery_presentation(current.value("ui").toObject());
    return QJsonDocument(presentation).toJson(QJsonDocument::Compact);
}
bool ConfigManager::saveWorkspaceRecovery(const FileSaveApplicationConfig& config,QString* error) const {
    const auto path=_config_path+".workspace.ini";
    if(QFileInfo(path).size()>workspace_recovery_limit)return recovery_failure(error,"Workspace recovery limit");
    QSettings settings(path,QSettings::IniFormat);settings.setFallbacksEnabled(false);settings.setAtomicSyncRequired(true);settings.sync();
    if(!recovery_file_is_writable(settings,path,error))return false;
    const auto bytes=encode_recovery_presentation(config);QJsonObject checked;
    if(!validate_recovery_presentation(bytes,checked,error))return false;
    return write_recovery_presentation(settings,_config_path,path,bytes,error);
}

bool ConfigManager::saveNumeric(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,QString* error) const {
    return saveNumeric(document,graph,desktop::NumericDraftPresentation{},error);
}

bool ConfigManager::saveNumeric(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,const desktop::NumericDraftPresentation& presentation,QString* error) const {
    desktop::NumericProjectDocument candidate;
    auto result=stage_numeric_candidate(document,graph,presentation,candidate);
    if(result)result=desktop::save_numeric_project(candidate,_config_path);
    if(error)*error=result.path.isEmpty()?result.error:result.path+": "+result.error;
    if(result)document=std::move(candidate);
    return static_cast<bool>(result);
}

bool ConfigManager::saveNumericAs(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,const QString& path,QString* error) {
    return saveNumericAs(document,graph,desktop::NumericDraftPresentation{},path,error);
}

bool ConfigManager::saveNumericAs(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,const desktop::NumericDraftPresentation& presentation,const QString& path,QString* error) {
    if(path.trimmed().isEmpty()){if(error)*error="Configuration path is empty";return false;}
    desktop::NumericProjectDocument candidate;
    auto result=stage_numeric_candidate(document,graph,presentation,candidate);
    if(result)result=desktop::save_numeric_project(candidate,desktop::numeric_project_path(path));
    if(error)*error=result.path.isEmpty()?result.error:result.path+": "+result.error;
    if(result){_config_path=candidate.active_path;document=std::move(candidate);}
    return static_cast<bool>(result);
}

QString ConfigManager::getConfigPath() const {
    return _config_path;
}

void ConfigManager::setConfigPath(const QString &path) {
    _config_path = desktop::numeric_project_path(path);
}

auto ConfigManager::_parse_core_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config) -> void {
    constexpr FileCoreApplicationConfig default_;
    if (not json_obj.contains("core"))
        return;
    const QJsonObject agg_obj = json_obj["core"].toObject();
    config.core.run_loopback = agg_obj.value("run_loopback").toBool(default_.run_loopback);
    config.core.run_analyser = agg_obj.value("run_analyser").toBool(default_.run_analyser);
    config.core.capture_source = std::clamp(agg_obj.value("capture_source").toInt(0), 0, 1);
    config.core.spectrum_channel = std::clamp(agg_obj.value("spectrum_channel").toInt(0), 0, 2);
    const auto merge = agg_obj.value("spectrum_merge").toString("energy");
    config.core.spectrum_merge = merge == "mean"      ? SpectrumMerge::Mean
                                 : merge == "maximum" ? SpectrumMerge::Maximum
                                                      : SpectrumMerge::Energy;
}

namespace {
void parse_ui_visibility(const QJsonObject& value,UIApplicationConfig& config) {
    const UIApplicationConfig defaults;
    config.show_raw_samples=value.value("show_raw_samples").toBool(defaults.show_raw_samples);
    config.show_window_signal=value.value("show_window_signal").toBool(defaults.show_window_signal);
    config.show_windowed_signal=value.value("show_windowed_signal").toBool(defaults.show_windowed_signal);
    config.show_window_function=value.value("show_window_function").toBool(defaults.show_window_function);
    config.show_amplitudes=value.value("show_amplitudes").toBool(defaults.show_amplitudes);
    config.show_test_amplitudes=value.value("show_test_amplitudes").toBool(defaults.show_test_amplitudes);
    config.show_spectrogram=value.value("show_spectrogram").toBool(defaults.show_spectrogram);
    config.show_data_flow=value.value("show_data_flow").toBool(true);
    config.show_fps=value.value("show_fps").toBool(true);
}
void parse_ui_preferences(const QJsonObject& value,UIApplicationConfig& config) {
    const UIApplicationConfig defaults;
    config.move_to_primary_screen_on_start=value.value("move_to_primary_screen_on_start").toBool(defaults.move_to_primary_screen_on_start);
    config.autosave_enabled=value.value("autosave_enabled").toBool(defaults.autosave_enabled);
    config.language=value.value("language").toString("ru");
    if(config.language!="ru" && config.language!="en")config.language="ru";
    config.theme=value.value("theme").toString("system");
    if(config.theme!="dark" && config.theme!="light")config.theme="system";
    config.pipeline_auto_apply=value.value("pipeline_auto_apply").toBool(true);
}
void parse_ui_workspace(const QJsonObject& value,UIApplicationConfig& config) {
    config.workspace=value.value("workspace").toObject();
    config.graph_views=value.value("graph_views").toObject();
    config.graph_rendering=value.value("graph_rendering").toObject();
    config.pipeline_presets=value.value("pipeline_presets").toArray();
    config.editor_view=value.value("editor_view").toObject();
    config.camera=value.value("camera").toObject();
    config.camera_controls=CameraControls::normalized(value.value("camera_controls").toObject());
    const auto ratio=value.value("spectrogram_side_panel_ratio").toDouble(.25);
    config.spectrogram_side_panel_ratio=ratio>=.01 && ratio<=.99?static_cast<float>(ratio):.25f;
}
void save_ui_visibility(QJsonObject& value,const UIApplicationConfig& config) {
    value["show_raw_samples"]=config.show_raw_samples;
    value["show_window_signal"]=config.show_window_signal;
    value["show_windowed_signal"]=config.show_windowed_signal;
    value["show_window_function"]=config.show_window_function;
    value["show_amplitudes"]=config.show_amplitudes;
    value["show_test_amplitudes"]=config.show_test_amplitudes;
    value["show_spectrogram"]=config.show_spectrogram;
    value["show_data_flow"]=config.show_data_flow;
    value["show_fps"]=config.show_fps;
}
void save_ui_preferences(QJsonObject& value,const UIApplicationConfig& config) {
    value["move_to_primary_screen_on_start"]=config.move_to_primary_screen_on_start;
    value["autosave_enabled"]=config.autosave_enabled;
    value["language"]=config.language;value["theme"]=config.theme;
    value["pipeline_auto_apply"]=config.pipeline_auto_apply;
}
void save_ui_workspace(QJsonObject& value,const UIApplicationConfig& config) {
    value["workspace"]=config.workspace;value["graph_views"]=config.graph_views;
    value["graph_rendering"]=config.graph_rendering;value["pipeline_presets"]=config.pipeline_presets;
    value["editor_view"]=config.editor_view;value["camera"]=config.camera;
    value["camera_controls"]=config.camera_controls;value["spectrogram_side_panel_ratio"]=config.spectrogram_side_panel_ratio;
}
}
void ConfigManager::_parse_ui_config(const QJsonObject& json_obj,FileSaveApplicationConfig& config) {
    if(!json_obj.contains("ui"))return;
    const auto value=json_obj.value("ui").toObject();
    parse_ui_visibility(value,config.ui);parse_ui_preferences(value,config.ui);parse_ui_workspace(value,config.ui);
}
void ConfigManager::_save_aggregator_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config) {
    QJsonObject agg_obj;
    agg_obj["run_loopback"] = config.core.run_loopback;
    agg_obj["run_analyser"] = config.core.run_analyser;
    agg_obj["capture_source"] = config.core.capture_source;
    agg_obj["spectrum_channel"] = config.core.spectrum_channel;
    agg_obj["spectrum_merge"] = config.core.spectrum_merge == SpectrumMerge::Mean      ? "mean"
                                : config.core.spectrum_merge == SpectrumMerge::Maximum ? "maximum"
                                                                                       : "energy";
    json_obj["core"] = agg_obj;
}

void ConfigManager::_save_ui_config(QJsonObject& json_obj,const FileSaveApplicationConfig& config) {
    QJsonObject value;
    save_ui_visibility(value,config.ui);save_ui_preferences(value,config.ui);save_ui_workspace(value,config.ui);
    json_obj["ui"]=value;
}
void ConfigManager::_parse_wled_ar_server_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config) {
    const WLEDARServerConfig default_;

    if (not json_obj.contains("wled_ar_server"))
        return;
    const QJsonObject wled_obj = json_obj["wled_ar_server"].toObject();
    config.wled_ar_server.enabled = wled_obj.value("enabled").toBool(default_.enabled);
    config.wled_ar_server.network_interface_name =
        wled_obj.value("network_interface_name").toString(default_.network_interface_name);
    const auto port = wled_obj.value("port").toInt(default_.port);
    config.wled_ar_server.port = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : default_.port;
}

void ConfigManager::_save_wled_ar_server_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config) {
    QJsonObject wled_obj;
    wled_obj["enabled"] = config.wled_ar_server.enabled;
    wled_obj["network_interface_name"] = config.wled_ar_server.network_interface_name;
    wled_obj["port"] = config.wled_ar_server.port;
    json_obj["wled_ar_server"] = wled_obj;
}
