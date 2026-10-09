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
#include <functional>
#include <map>
#include <pipeline/owned_frame.h>
#include <domain/processing_contracts.h>
class GuiPresenter;
class QVBoxLayout;
class QPushButton;
class QScrollArea;
class QAction;
class QToolButton;
class QSplitter;
class QMenu;

class SchemeEditorWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit SchemeEditorWidget(GuiPresenter &application, QWidget *parent = nullptr);
    ~SchemeEditorWidget() override;
    void flush_presentation();
    void arrange_nodes();
    void initialize_default_layout();
    bool save_preset(const QString &name);
    bool load_preset(const QString &id);
    bool remove_preset(const QString &id);
    QJsonObject document() const;
    QJsonObject graph_adapter_metadata() const;
    bool restore_graph_adapters(const QJsonObject &node_adapters,QString *error=nullptr);
    bool numeric_draft(domain::NumericProcessingGraph&,QString* error=nullptr) const;
    bool load_numeric_draft(const domain::NumericProcessingGraph&,QString* error=nullptr);
    void sync_preferences();
    void set_navigation_speed(float speed);
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
    void wire_source_model(QtNodes::NodeId id);
    QLabel *_status = nullptr;
    QLabel *_capture_status = nullptr;
    int _capture_state = 0;
    QString _capture_error;
    void refresh_capture_status();
    QPushButton *_apply_button = nullptr;
    QAction *_arrange_action = nullptr, *_center_action = nullptr;
    QToolButton *_presets_button = nullptr;
    QWidget *_toolbar = nullptr;
    bool _auto_apply = false;
    bool _live_apply_pending = false;
    QTimer _apply_timer, _save_timer;
    QJsonObject _last_processing_document;
    QJsonObject _last_adapter_metadata;
    QJsonObject _document_template;
    bool _unsupported_document = false;
    QJsonArray _unrendered_connections;
    void retain_unrendered_connections();
    QtNodes::NodeId _inspected_node = QtNodes::InvalidNodeId;
    QVBoxLayout *_inspector_layout = nullptr;
    QScrollArea *_inspector = nullptr;
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
    void load_active_pipeline();
    bool load_document(const QJsonObject &document, const QJsonObject &view = {}, bool active = false, bool switch_device = false);
    void layout_nodes(bool undoable);
    void fit_nodes();
    QRectF node_bounds() const;
    QtNodes::DataFlowGraphicsScene *_scene = nullptr;
    QtNodes::GraphicsView *_view = nullptr;
};

#endif // QT_VISUALIZATION_EDITOR_WIDGET_H
