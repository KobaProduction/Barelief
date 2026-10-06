#include "visualization/surface_settings.h"
#include "config/config_document.h"
#include "core_app/config_manager.h"

#include <QSaveFile>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QLoggingCategory>
#include <QDir>
#include <utility>

Q_DECLARE_LOGGING_CATEGORY(core_app_config)

Q_LOGGING_CATEGORY(core_app_config, "core_app.config")

ConfigManager::ConfigManager(QString config_path) : _config_path(QFileInfo(config_path).absoluteFilePath()) {}

bool ConfigManager::load(FileSaveApplicationConfig &config, QString *error) const {
    if (error)
        error->clear();
    const auto document = configuration::read(_config_path);
    if (!document) {
        if (error)
            *error = document.error;
        qCWarning(core_app_config, "Cannot read configuration: %s", qPrintable(document.error));
        return false;
    }
    const auto json_obj = document.document;
    config.raw_document = json_obj;

    _parse_core_config(json_obj, config);
    _parse_ui_config(json_obj, config);
    _parse_diagram_config(json_obj, config);
    config.pipeline_graph = json_obj.value("pipeline_graph").toObject();
    if (json_obj.value("nodes").isArray()) {
        config.pipeline_graph = json_obj;
        config.scene_graph = json_obj;
    }
    _parse_wled_ar_server_config(json_obj, config);

    qCInfo(core_app_config, "Config loaded successfully from: %s", qPrintable(_config_path));
    return true;
}

bool ConfigManager::save(const FileSaveApplicationConfig &config, QString *error) const {
    if (error)
        error->clear();
    // Создаем директорию если её нет
    const QFileInfo file_info(_config_path);
    const QDir dir = file_info.absoluteDir();
    if (not dir.exists() and not dir.mkpath(".")) {
        if (error)
            *error = "Could not create directory: " + dir.absolutePath();
        qCWarning(core_app_config, "Failed to create config directory: %s", qPrintable(dir.absolutePath()));
        return false;
    }

    QSaveFile config_file(_config_path);
    if (not config_file.open(QIODevice::WriteOnly)) {
        if (error)
            *error = config_file.errorString();
        qCWarning(core_app_config, "Failed to open config file for writing: %s", qPrintable(_config_path));
        return false;
    }

    QJsonObject json_obj;
    _save_aggregator_config(json_obj, config);
    _save_ui_config(json_obj, config);
    _save_scheme_config(json_obj, config);
    // Keep even malformed/non-object schema values verbatim during prefs writes.
    // Only a changed schema replaces the originally loaded value.
    if (!config.raw_document.contains("pipeline_graph") ||
        config.pipeline_graph != config.raw_document.value("pipeline_graph").toObject())
        json_obj["pipeline_graph"] = config.pipeline_graph;
    _save_wled_ar_server_config(json_obj, config);

    const QJsonDocument json_doc(configuration::overlay(config.raw_document, json_obj));
    const auto data = json_doc.toJson();
    if (config_file.write(data) != data.size() || !config_file.commit()) {
        if (error)
            *error = config_file.errorString();
        return false;
    }

    qCInfo(core_app_config, "Config saved successfully to: %s", qPrintable(_config_path));
    return true;
}

bool ConfigManager::saveAs(const FileSaveApplicationConfig &config, const QString &path, QString *error) {
    if (path.trimmed().isEmpty()) {
        if (error)
            *error = "Configuration path is empty";
        return false;
    }
    ConfigManager destination(path);
    if (!destination.save(config, error))
        return false;
    _config_path = destination.getConfigPath();
    return true;
}

QString ConfigManager::getConfigPath() const {
    return _config_path;
}

void ConfigManager::setConfigPath(const QString &path) {
    _config_path = QFileInfo(path).absoluteFilePath();
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
    const auto read_legacy = [&ui_obj](const char *name, const char *legacy) {
        return ui_obj.value(ui_obj.contains(name) ? name : legacy);
    };
    config.ui.move_to_primary_screen_on_start =
        ui_obj.value("move_to_primary_screen_on_start").toBool(default_.move_to_primary_screen_on_start);
    config.ui.autosave_enabled = ui_obj.value("autosave_enabled").toBool(default_.autosave_enabled);

    config.ui.show_raw_samples = ui_obj.value("show_raw_samples").toBool(default_.show_raw_samples);
    config.ui.show_window_signal =
        read_legacy("show_window_signal", "show_samples").toBool(default_.show_window_signal);
    config.ui.show_windowed_signal =
        read_legacy("show_windowed_signal", "show_samples_after_window_function").toBool(default_.show_windowed_signal);
    config.ui.show_window_function = ui_obj.value("show_window_function").toBool(default_.show_window_function);
    config.ui.show_amplitudes = ui_obj.value("show_amplitudes").toBool(default_.show_amplitudes);
    config.ui.show_test_amplitudes = ui_obj.value("show_test_amplitudes").toBool(default_.show_test_amplitudes);
    config.ui.show_spectrogram = read_legacy("show_spectrogram", "show_surface").toBool(default_.show_spectrogram);
    config.ui.workspace = ui_obj.value("workspace").toObject();
    config.ui.language = ui_obj.value("language").toString("ru");
    if (config.ui.language != "ru" && config.ui.language != "en")
        config.ui.language = "ru";
    config.ui.theme = ui_obj.value("theme").toString("system");
    if (config.ui.theme != "dark" && config.ui.theme != "light")
        config.ui.theme = "system";
    config.ui.graph_views = ui_obj.value("graph_views").toObject();
    config.ui.graph_rendering = ui_obj.value("graph_rendering").toObject();
    if (!config.ui.graph_rendering.contains("version")) {
        auto defaults = SurfaceSettings::defaults();
        const auto old = config.ui.graph_views.value("spectrogram").toObject().value("renderer").toObject();
        if (old.contains("gradient"))
            defaults["gradient"] = old.value("gradient").toInt(1) == 0 ? "black_yellow" : "green_red";
        config.ui.graph_rendering["version"] = 1;
        const auto savedDefaults = config.ui.graph_rendering.value("defaults").toObject();
        for (auto it = savedDefaults.begin(); it != savedDefaults.end(); ++it)
            defaults[it.key()] = it.value();
        config.ui.graph_rendering["defaults"] = SurfaceSettings::normalized(defaults);
        for (auto it = config.ui.graph_views.begin(); it != config.ui.graph_views.end(); ++it) {
            auto view = it.value().toObject();
            auto renderer = view.value("renderer").toObject();
            if (it.key() != "spectrogram" && renderer.contains("gradient"))
                view["surface_override"] = QJsonObject{
                    {"enabled", true},
                    {"values", QJsonObject{{"gradient",
                                            renderer.value("gradient").toInt(1) == 0 ? "black_yellow" : "green_red"}}}};
            if (renderer.contains("gradient")) {
                renderer.remove("gradient");
                view["renderer"] = renderer;
                it.value() = view;
            }
        }
    }

    if (!config.ui.graph_views.contains("windowSignal") && config.ui.graph_views.contains("samples"))
        config.ui.graph_views["windowSignal"] = config.ui.graph_views.value("samples");
    if (!config.ui.graph_views.contains("windowedSignal") && config.ui.graph_views.contains("windowedSamples"))
        config.ui.graph_views["windowedSignal"] = config.ui.graph_views.value("windowedSamples");
    config.ui.pipeline_auto_apply = ui_obj.value("pipeline_auto_apply").toBool(true);
    config.ui.show_fps = ui_obj.value("show_fps").toBool(true);
    config.ui.pipeline_presets = ui_obj.value("pipeline_presets").toArray();
    config.ui.editor_view = ui_obj.value("editor_view").toObject();
    config.ui.show_data_flow = ui_obj.value("show_data_flow").toBool(true);
    config.ui.camera = ui_obj.value("camera").toObject();
    config.ui.camera_controls = CameraControls::normalized(ui_obj.value("camera_controls").toObject());
    const auto ratio = read_legacy("spectrogram_side_panel_ratio", "surface_side_panel_ratio").toDouble(0.25);
    config.ui.spectrogram_side_panel_ratio = ratio >= 0.01 && ratio <= 0.99 ? static_cast<float>(ratio) : 0.25f;
}

void ConfigManager::_parse_diagram_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config) {
    config.scene_graph = json_obj.value("scene_graph").toObject(QJsonObject());
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

void ConfigManager::_save_scheme_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config) {
    if (!config.raw_document.contains("scene_graph") ||
        config.scene_graph != config.raw_document.value("scene_graph").toObject())
        json_obj["scene_graph"] = config.scene_graph;
}

void ConfigManager::_parse_wled_ar_server_config(const QJsonObject &json_obj, FileSaveApplicationConfig &config) {
    const WLEDARServerConfig default_;

    if (not json_obj.contains("wled_ar_server"))
        return;
    const QJsonObject wled_obj = json_obj["wled_ar_server"].toObject();
    config.wled_ar_server.enabled = wled_obj.value("enabled").toBool(default_.enabled);
    config.wled_ar_server.network_interface_name =
        wled_obj.value("network_interface_name").toString(default_.network_interface_name);
    const auto port = wled_obj.value("port").toInt(wled_audio_reactive::DEFAULT_PORT);
    config.wled_ar_server.port = port >= 1 && port <= 65535 ? static_cast<quint16>(port) : default_.port;
}

void ConfigManager::_save_wled_ar_server_config(QJsonObject &json_obj, const FileSaveApplicationConfig &config) {
    QJsonObject wled_obj;
    wled_obj["enabled"] = config.wled_ar_server.enabled;
    wled_obj["network_interface_name"] = config.wled_ar_server.network_interface_name;
    wled_obj["port"] = config.wled_ar_server.port;
    json_obj["wled_ar_server"] = wled_obj;
}
