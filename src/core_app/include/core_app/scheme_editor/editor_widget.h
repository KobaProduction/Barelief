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

signals:
    void status_message(const QString &text);

  private:
    GuiPresenter &_application;
    quint64 _apply_request = 0;
    QLabel *_status = nullptr;
    QTimer _apply_timer, _save_timer;
    QJsonObject _last_processing_document;
    bool _loading = false;
    QSet<QtNodes::NodeId> _known_nodes;
    void changed();
    void set_status(const QString &text);
    void save_view();
    void avoid_overlap(QtNodes::NodeId id);
    QElapsedTimer _preview_clock;
    void apply();
    void load_active_pipeline();
    void load_document(const QJsonObject &document);
    QtNodes::DataFlowGraphicsScene *_scene = nullptr;
    QtNodes::GraphicsView *_view = nullptr;
};

#endif // QT_VISUALIZATION_EDITOR_WIDGET_H
