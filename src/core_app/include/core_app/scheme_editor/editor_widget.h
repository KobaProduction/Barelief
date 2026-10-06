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
class GuiPresenter;
class QVBoxLayout;
class QPushButton;
class QScrollArea;
class QAction;
class QToolButton;
class QSplitter;

class SchemeEditorWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit SchemeEditorWidget(GuiPresenter &application, QWidget *parent = nullptr);
    ~SchemeEditorWidget() override;
    void flush_presentation();
    void arrange_nodes();
    bool save_preset(const QString &name);
    bool load_preset(const QString &id);
    bool remove_preset(const QString &id);
    QJsonObject document() const;
    void cancel_start_intent();
    void sync_preferences();
    void set_navigation_speed(float speed);
    QAction *arrange_action() const { return _arrange_action; }
    QAction *center_action() const { return _center_action; }
    QToolButton *presets_button() const { return _presets_button; }

signals:
    void status_message(const QString &text);
    void source_start_requested();
    void graph_view_requested(int node_id);
    void graph_window_requested(int node_id);
    void graph_open_requested(int node_id,int placement);
protected:
    void changeEvent(QEvent *event) override;

  private:
    GuiPresenter &_application;
    quint64 _apply_request = 0;
    bool _default_start_intent = false;
    QSet<QtNodes::NodeId> _source_start_intents;
    void wire_source_model(QtNodes::NodeId id);
    void source_output_changed(bool enabled);
    QLabel *_status = nullptr;
    QPushButton *_apply_button = nullptr;
    QAction *_arrange_action = nullptr, *_center_action = nullptr;
    QToolButton *_presets_button = nullptr;
    bool _auto_apply = false;
    QTimer _apply_timer, _save_timer;
    QJsonObject _last_processing_document;
    QJsonObject _document_template;
    bool _unsupported_document = false;
    QJsonArray _unrendered_connections;
    void retain_unrendered_connections();
    QtNodes::NodeId _inspected_node = QtNodes::InvalidNodeId;
    QVBoxLayout *_inspector_layout = nullptr;
    QScrollArea *_inspector = nullptr;
    void inspect_node(QtNodes::NodeId id);
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
    void apply();
    void load_active_pipeline();
    void load_document(const QJsonObject &document, const QJsonObject &view = {}, bool active = false, bool switch_device = false);
    void layout_nodes(bool undoable);
    void fit_nodes();
    QRectF node_bounds() const;
    QtNodes::DataFlowGraphicsScene *_scene = nullptr;
    QtNodes::GraphicsView *_view = nullptr;
};

#endif // QT_VISUALIZATION_EDITOR_WIDGET_H
