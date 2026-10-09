#ifndef CORE_APP_TYPES_H
#define CORE_APP_TYPES_H

#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/NodeDelegateModelRegistry>
#include <QJsonObject>
#include <QJsonArray>
#include "visualization/camera_controls.h"

#include "pipeline/engine_models.h"


using node_delegate_model_registry_t = std::shared_ptr<QtNodes::NodeDelegateModelRegistry>;
using data_flow_graph_model_t = std::shared_ptr<QtNodes::DataFlowGraphModel>;

struct UIApplicationConfig {
    bool move_to_primary_screen_on_start = false;
    bool autosave_enabled = true;
    bool show_raw_samples = false;
    bool show_window_signal = false;
    bool show_windowed_signal = false;
    bool show_window_function = false;
    bool show_amplitudes = false;
    bool show_test_amplitudes = false;
    bool show_spectrogram = true;
    QJsonObject workspace;
    QString language = "ru";
    QString theme = "system";
    QJsonObject graph_views;
    QJsonObject graph_rendering;
    QJsonObject editor_view;
    bool show_data_flow = true;
    bool pipeline_auto_apply = true;
    bool show_fps = true;
    QJsonArray pipeline_presets;
    QJsonObject camera;
    QJsonObject camera_controls = CameraControls::defaults();
    float spectrogram_side_panel_ratio = 0.25f;
};

struct WLEDARServerConfig {
    bool enabled = false;
    QString network_interface_name{};
    quint16 port = 11988;
};

struct FileCoreApplicationConfig {
    bool run_loopback = true;
    bool run_analyser = true;
    int capture_source = 0;
    int spectrum_channel = 0;
    SpectrumMerge spectrum_merge = SpectrumMerge::Energy;
};

struct FileSaveApplicationConfig {
    QJsonObject raw_document; // Preserve extension fields when updating known settings.
    FileCoreApplicationConfig core;
    UIApplicationConfig ui{};
    WLEDARServerConfig wled_ar_server{};
    QJsonObject scene_graph;    // Editable draft, including incomplete connections.
    QJsonObject pipeline_graph; // Persisted audio graph; runtime snapshots may differ.
};

struct CoreApplicationConfig {
    bool enable_ui = true;
};

// Mutable GUI/editor state contains no engine objects.
struct GuiState {
    CoreApplicationConfig configs;
    FileSaveApplicationConfig file_configs;
    node_delegate_model_registry_t delegate_model_registry;
    data_flow_graph_model_t data_flow_graph;
    // Only editor edits / an explicit Save may replace the saved draft.
    bool editor_draft_modified = false;
};

using CoreApplicationContext = GuiState;

#endif // CORE_APP_TYPES_H
