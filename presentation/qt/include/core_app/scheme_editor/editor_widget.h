#ifndef QT_VISUALIZATION_EDITOR_WIDGET_H
#define QT_VISUALIZATION_EDITOR_WIDGET_H

#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtCore/QString>

#include "core_app/types.h"
#include <QLabel>
#include <QTimer>
#include <QElapsedTimer>
#include <QSet>
#include <QMap>
#include <functional>
#include <map>
#include <pipeline/owned_frame.h>
#include <domain/processing_contracts.h>
namespace pipeline { struct DeliveredNumericHistoryRow; }
class QHBoxLayout;
class QFormLayout;
class QLineEdit;
class QKeySequence;
class GuiPresenter;
class AudioNodeModel;
class NumericEditorGraphModel;
class QVBoxLayout;
class QPushButton;
class QScrollArea;
class QAction;
class QToolButton;
class QSplitter;
class QMenu;
class QTreeWidget;
class QJsonArray;

class SchemeEditorWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit SchemeEditorWidget(GuiPresenter &application, QWidget *parent = nullptr);
    ~SchemeEditorWidget() override;
    void flush_presentation();
    void arrange_nodes();
    void initialize_default_layout();
    void restore_node_positions(const QJsonObject&);
    bool save_preset(const QString &name);
    bool load_preset(const QString &id);
    bool remove_preset(const QString &id);
    QJsonObject document() const;
    QJsonObject graph_adapter_metadata() const;
    bool restore_graph_adapters(const QJsonObject &node_adapters,QString *error=nullptr);
    bool numeric_draft(domain::NumericProcessingGraph&,QString* error=nullptr,bool resolve_native=true) const;
    bool load_numeric_draft(const domain::NumericProcessingGraph&,QString* error=nullptr,const QJsonObject& node_adapters={});
    void sync_preferences();
    void set_navigation_speed(float speed);
    QWidget* execution_settings_widget(QWidget* parent=nullptr);
    QAction *arrange_action() const { return _arrange_action; }
    QAction *center_action() const { return _center_action; }
    QToolButton *presets_button() const { return _presets_button; }
    QWidget *toolbar() const { return _toolbar; }
    QUndoStack &history() const { return _scene->undoStack(); }
    void set_graph_menu_provider(std::function<void(int,QMenu *)> provider);
    void set_preset_presentation_provider(std::function<QJsonObject()> provider);

signals:
    void default_graph_requested();
    void generator_loopback_graph_requested();
    void numeric_draft_changed();
    void execution_settings_changed();
    void status_message(const QString &text);
    void graph_view_requested(int node_id);
    void graph_rename_requested(int node_id);
    void graph_window_requested(int node_id);
    void graph_open_requested(int node_id,int placement);
    void preset_presentation_requested(const QJsonObject &presentation);
protected:
    void changeEvent(QEvent *event) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
    bool eventFilter(QObject*,QEvent*) override;

  private:
    GuiPresenter &_application;
    std::function<QJsonObject()> _preset_presentation_provider;
    quint64 _apply_request = 0;
    QHBoxLayout* create_toolbar(QVBoxLayout*);
    void setup_editor_chrome();
    void initialize_preview_ui();
    QAction* canvas_shortcut(const char*,const char*,const QKeySequence&,void(SchemeEditorWidget::*)());
    int selected_graph() const;
    void setup_graph_shortcut(const char*,const char*,const QKeySequence&,int);
    void open_selected_graph(int placement);
    void setup_canvas_actions(QHBoxLayout*,QPushButton*);
    void bind_view_interactions();
    void bind_view_camera_saving();
    void synchronize_moved_positions();
    void refresh_library_language(QTreeWidget* library);
    void inspect_scene_selection();
    void refresh_graph_indicators();
    void refresh_connection_diagnostics();
    void refresh_node_connection_diagnostics(NumericEditorGraphModel*,QtNodes::NodeId);
    void set_editor_display_setting(const QString& key,int value);
    void on_numeric_preview(const pipeline::DeliveredNumericFrame& frame);
    void on_history_preview(const pipeline::DeliveredNumericHistoryRow& row);
    void on_snapshot_preview(const pipeline::DeliveredNumericFrame& frame,quint64 ordinal,quint64 filled_count);
    void clear_node_previews();
    void on_graph_apply_finished(quint64 request,bool accepted,const QString& error);
    void restore_saved_camera(quint64 generation);
    bool refresh_processing_rate_label(const QJsonObject& telemetry);
    void refresh_node_processing_activity(const QJsonArray& stats,bool current,bool measured);
    void apply_initial_layout(quint64 generation);
    bool preview_matches_active(const pipeline::DeliveredNumericFrame& frame) const;
    bool preview_port_available(const pipeline::DeliveredNumericFrame& frame) const;
    using AdapterMap=QMap<QtNodes::NodeId,QJsonObject>;
    bool decode_graph_adapters(const QJsonObject& node_adapters,AdapterMap& checked,QString* error) const;
    bool apply_graph_adapters(const AdapterMap& checked,QString* error);
    void restore_previous_adapters(const AdapterMap&);
    bool prepare_draft_import(NumericEditorGraphModel* numeric,const domain::NumericProcessingGraph& graph,
                              const QJsonObject& adapters,domain::NumericProcessingGraph& prepared,QString* error);
    bool commit_draft_import(NumericEditorGraphModel* numeric,const domain::NumericProcessingGraph& prepared,
                             const QJsonObject& adapters,QString* error);
    void finish_draft_import(NumericEditorGraphModel* numeric);
    void populate_node_inspector(AudioNodeModel* model,QtNodes::NodeId id);
    void populate_empty_inspector();
    bool add_preset_presentation(QJsonObject& preset) const;
    void record_loaded_document(bool active,bool switch_device);
    void record_processing_edit(const QJsonObject& processing);
    void record_adapter_edit();
    void setup_validation_status(QHBoxLayout*);
    void setup_capture_status(QHBoxLayout*);
    void bind_apply_timer();
    void bind_save_timer();
    void setup_processing_rate(QHBoxLayout*);
    void setup_zoom_control(QHBoxLayout*);
    QPushButton* setup_center_control(QHBoxLayout*);
    void setup_splitter(QVBoxLayout*);
    void setup_node_library(QSplitter*);
    void setup_inspector(QSplitter*);
    void bind_scene_selection();
    QMenu* create_preset_menu();
    void request_builtin(bool);
    void populate_user_presets(QMenu*,QMenu*);
    bool load_selected_preset(const QJsonObject&);
    void setup_user_presets(QMenu*);
    void setup_presets();
    void bind_graph_model();
    void bind_graph_changes();
    void bind_connection_diagnostics();
    void setup_editor_display(QHBoxLayout*);
    void add_editor_display_control(QFormLayout*,QWidget*,const char*,const char*,int,int,int);
    void initialize_document();
    void configure_scene();
    void configure_view();
    void initialize_preview_tracking();
    bool active_adapters_match(const QJsonObject&) const;
    void reconcile_pending_apply(const QJsonObject&,const QJsonObject&);
    void observe_active_graph(const QJsonObject&);
    void bind_preview_delivery();
    void bind_application_feedback();
    void restore_initial_camera();
    void apply_saved_camera(const QJsonObject& saved);
    void refresh_execution_rate_text(NumericEditorGraphModel*,QLineEdit*);
    void wire_source_model(QtNodes::NodeId id);
    QLabel *_status = nullptr;
    QLabel *_capture_status = nullptr;
    QLabel *_processing_rate = nullptr;
    void refresh_processing_telemetry();
    int _capture_state = 0;
    QString _capture_error;
    void refresh_capture_status();
    QPushButton *_apply_button = nullptr;
    QAction *_arrange_action = nullptr, *_center_action = nullptr;
    QToolButton *_presets_button = nullptr;
    QWidget *_toolbar = nullptr;
    bool _auto_apply = false;
    bool _live_apply_pending = false;
    bool _immediate_apply_pending = false;
    QTimer _apply_timer, _save_timer;
    QJsonObject _last_processing_document;
    QJsonObject _last_adapter_metadata;
    QJsonObject _document_template;
    bool _unsupported_document = false;
    QJsonArray _unrendered_connections;
    void retain_unrendered_connections();
    QtNodes::NodeId _inspected_node = QtNodes::InvalidNodeId;
    QSet<QtNodes::NodeId> _preview_nodes;
    QVBoxLayout *_inspector_layout = nullptr;
    QScrollArea *_inspector = nullptr;
    void clear_inspector();
    void append_inspector_title(AudioNodeModel*);
    void append_input_summary(AudioNodeModel*);
    void rename_node(QtNodes::NodeId);
    void append_rename_action(AudioNodeModel*,QtNodes::NodeId);
    void refresh_node_timing(QtNodes::NodeId,QLabel*);
    void append_node_diagnostics(QtNodes::NodeId);
    void inspect_node(QtNodes::NodeId id);
    void inspect_connection(QtNodes::ConnectionId);
    QSplitter *_splitter = nullptr;
    int _inspector_width = 280;
    void set_inspector_expanded(bool expanded);
    void update_graph_view_indicator(QtNodes::NodeId id);
    bool _loading = false;
    quint64 _document_generation = 0;
    void changed();
    void set_status(const QString &text);
    void save_view();
    QElapsedTimer _preview_clock;
    std::map<pipeline::EndpointIdentity,qint64> _preview_times;
    QJsonObject _active_processing_document;
    QJsonObject _active_source_adapters;
    void record_active_source_adapters(const QJsonObject&);
    bool preview_source_active(QtNodes::NodeId) const;
    bool accept_preview(const pipeline::DeliveredNumericFrame&,bool throttle);
    void sync_preview_subscription();
    void refresh_preview_activity();
    int _preview_engine_state = 0;
    std::uint64_t _preview_revision = 0;
    std::uint64_t _preview_epoch = 0;
    void clear_previews();
    bool _initial_layout_pending=false,_initial_layout_scheduled=false,_initial_layout_fit=true;
    void schedule_initial_layout();
    void apply();
    void schedule_apply();
    void load_active_pipeline();
    bool prepare_document_visual(const QJsonObject& document, bool active, QJsonObject& visual);
    void begin_document_load(const QJsonObject& visual, bool switch_device);
    void rebuild_document_graph();
    void restore_document_view(const QJsonObject& view);
    void finish_document_load(bool active, bool switch_device);

    bool load_document(const QJsonObject &document, const QJsonObject &view = {}, bool active = false, bool switch_device = false);
    void layout_nodes(bool undoable);
    void fit_nodes();
    QRectF node_bounds() const;
    QtNodes::DataFlowGraphicsScene *_scene = nullptr;
    QtNodes::GraphicsView *_view = nullptr;
};

#endif // QT_VISUALIZATION_EDITOR_WIDGET_H
