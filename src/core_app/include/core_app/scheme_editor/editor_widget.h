#ifndef QT_VISUALIZATION_EDITOR_WIDGET_H
#define QT_VISUALIZATION_EDITOR_WIDGET_H

#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtCore/QString>

#include "core_app/types.h"
#include <QLabel>
#include <QTimer>
#include <QElapsedTimer>
class GuiPresenter;

class SchemeEditorWidget final : public QWidget {
    Q_OBJECT

  public:
    explicit SchemeEditorWidget(GuiPresenter &application, QWidget *parent = nullptr);
    ~SchemeEditorWidget() override = default;

  private:
    GuiPresenter &_application;
    quint64 _apply_request = 0;
    QLabel *_status = nullptr;
    QTimer _apply_timer;
    QElapsedTimer _preview_clock;
    void apply();
    void load_active_pipeline();
    QtNodes::DataFlowGraphicsScene *_scene = nullptr;
    QtNodes::GraphicsView *_view = nullptr;
};

#endif // QT_VISUALIZATION_EDITOR_WIDGET_H
