

#include "core_app/config_manager.h"


#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>

#include <QDir>
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

bool ConfigManager::saveNumeric(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,QString* error) const {
    return saveNumeric(document,graph,desktop::NumericDraftPresentation{},error);
}

bool ConfigManager::saveNumeric(desktop::NumericProjectDocument& document,const domain::NumericProcessingGraph& graph,const desktop::NumericDraftPresentation& presentation,QString* error) const {
    desktop::NumericProjectDocument candidate;
    auto result=desktop::stage_numeric_draft(document,graph,candidate);
    if(result && (!presentation.positions.isEmpty() || !presentation.node_presentations.isEmpty() || !presentation.node_adapters.isEmpty() || !presentation.editor_view.isEmpty() || !presentation.application_settings.isEmpty() || presentation.applied_graph))result=desktop::stage_numeric_presentation(candidate,presentation,candidate);
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
    auto result=desktop::stage_numeric_draft(document,graph,candidate);
    if(result && (!presentation.positions.isEmpty() || !presentation.node_presentations.isEmpty() || !presentation.node_adapters.isEmpty() || !presentation.editor_view.isEmpty() || !presentation.application_settings.isEmpty() || presentation.applied_graph))result=desktop::stage_numeric_presentation(candidate,presentation,candidate);
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

void ConfigManager::_parse_ui_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config) {
    const UIApplicationConfig default_;

    if (not json_obj.contains("ui"))
        return;
    const QJsonObject ui_obj = json_obj["ui"].toObject();
    config.ui.move_to_primary_screen_on_start =
        ui_obj.value("move_to_primary_screen_on_start").toBool(default_.move_to_primary_screen_on_start);
    config.ui.autosave_enabled = ui_obj.value("autosave_enabled").toBool(default_.autosave_enabled);

    config.ui.show_raw_samples = ui_obj.value("show_raw_samples").toBool(default_.show_raw_samples);
    config.ui.show_window_signal =
        ui_obj.value("show_window_signal").toBool(default_.show_window_signal);
    config.ui.show_windowed_signal =
        ui_obj.value("show_windowed_signal").toBool(default_.show_windowed_signal);
    config.ui.show_window_function = ui_obj.value("show_window_function").toBool(default_.show_window_function);
    config.ui.show_amplitudes = ui_obj.value("show_amplitudes").toBool(default_.show_amplitudes);
    config.ui.show_test_amplitudes = ui_obj.value("show_test_amplitudes").toBool(default_.show_test_amplitudes);
    config.ui.show_spectrogram = ui_obj.value("show_spectrogram").toBool(default_.show_spectrogram);
    config.ui.workspace = ui_obj.value("workspace").toObject();
    config.ui.language = ui_obj.value("language").toString("ru");
    if (config.ui.language != "ru" && config.ui.language != "en")
        config.ui.language = "ru";
    config.ui.theme = ui_obj.value("theme").toString("system");
    if (config.ui.theme != "dark" && config.ui.theme != "light")
        config.ui.theme = "system";
    config.ui.graph_views = ui_obj.value("graph_views").toObject();
    config.ui.graph_rendering = ui_obj.value("graph_rendering").toObject();
    config.ui.pipeline_auto_apply = ui_obj.value("pipeline_auto_apply").toBool(true);
    config.ui.show_fps = ui_obj.value("show_fps").toBool(true);
    config.ui.pipeline_presets = ui_obj.value("pipeline_presets").toArray();
    config.ui.editor_view = ui_obj.value("editor_view").toObject();
    config.ui.show_data_flow = ui_obj.value("show_data_flow").toBool(true);
    config.ui.camera = ui_obj.value("camera").toObject();
    config.ui.camera_controls = CameraControls::normalized(ui_obj.value("camera_controls").toObject());
    const auto ratio = ui_obj.value("spectrogram_side_panel_ratio").toDouble(0.25);
    config.ui.spectrogram_side_panel_ratio = ratio >= 0.01 && ratio <= 0.99 ? static_cast<float>(ratio) : 0.25f;
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

void ConfigManager::_save_ui_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config) {
    QJsonObject ui_obj;
    ui_obj["move_to_primary_screen_on_start"] = config.ui.move_to_primary_screen_on_start;
    ui_obj["autosave_enabled"] = config.ui.autosave_enabled;
    ui_obj["show_raw_samples"] = config.ui.show_raw_samples;
    ui_obj["show_window_signal"] = config.ui.show_window_signal;
    ui_obj["show_windowed_signal"] = config.ui.show_windowed_signal;
    ui_obj["show_window_function"] = config.ui.show_window_function;
    ui_obj["show_raw_samples"] = config.ui.show_raw_samples;
    ui_obj["show_amplitudes"] = config.ui.show_amplitudes;
    ui_obj["show_test_amplitudes"] = config.ui.show_test_amplitudes;
    ui_obj["show_spectrogram"] = config.ui.show_spectrogram;
    ui_obj["workspace"] = config.ui.workspace;
    ui_obj["language"] = config.ui.language;
    ui_obj["theme"] = config.ui.theme;
    ui_obj["graph_views"] = config.ui.graph_views;
    ui_obj["graph_rendering"] = config.ui.graph_rendering;
    ui_obj["pipeline_auto_apply"] = config.ui.pipeline_auto_apply;
    ui_obj["show_fps"] = config.ui.show_fps;
    ui_obj["pipeline_presets"] = config.ui.pipeline_presets;
    ui_obj["editor_view"] = config.ui.editor_view;
    ui_obj["show_data_flow"] = config.ui.show_data_flow;
    ui_obj["camera"] = config.ui.camera;
    ui_obj["camera_controls"] = config.ui.camera_controls;
    ui_obj["spectrogram_side_panel_ratio"] = config.ui.spectrogram_side_panel_ratio;
    json_obj["ui"] = ui_obj;
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
