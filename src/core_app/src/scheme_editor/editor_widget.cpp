#include "visualization/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/editor_widget.h"

#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/StyleCollection>
#include <QApplication>
#include <QtNodes/DataFlowGraphModel>
#include <QtNodes/DataFlowGraphicsScene>
#include <QtNodes/GraphicsView>
#include <QtNodes/NodeDelegateModelRegistry>

#include <QtWidgets/QMessageBox>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QStandardPaths>
#include <QtCore/QDir>
#include <QtCore/QDebug>
#include <QtGui/QKeyEvent>

#include "core_app/gui_presenter.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QUndoStack>
#include <QUndoCommand>
#include <QScrollBar>
#include <QJsonArray>
#include <QWheelEvent>
#include <QPainter>
#include <QDoubleSpinBox>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QGraphicsProxyWidget>
#include <QToolButton>
#include <QMenu>
#include <QStyle>
#include <QClipboard>
#include <QMimeData>
#include <QInputDialog>
#include <QUuid>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QtNodes/internal/UndoCommands.hpp>
#include <QTreeWidget>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include "pipeline/test_signal.h"
#include <map>
#include <queue>
#include <algorithm>
#include <array>
#include <cmath>
#include "core_app/scheme_editor/sink_data_model.h"
#include "core_app/scheme_editor/source_data_model.h"
#include "core_app/scheme_editor/processor_data_model.h"

using QtNodes::ConnectionStyle;
using QtNodes::DataFlowGraphicsScene;
using QtNodes::DataFlowGraphModel;
using QtNodes::GraphicsView;
using QtNodes::NodeDelegateModelRegistry;

class InspectorSplitter final : public QSplitter {
public:
    using QSplitter::QSplitter;
    std::function<void()> toggle_right;
protected:
    class Handle final : public QSplitterHandle {
    public:
        Handle(Qt::Orientation orientation,QSplitter *parent) : QSplitterHandle(orientation,parent) {}
        std::function<void()> clicked;
    protected:
        void mousePressEvent(QMouseEvent *event) override {
            _press=event->position().toPoint(); _dragged=false; _left=event->button()==Qt::LeftButton;
            QSplitterHandle::mousePressEvent(event);
        }
        void mouseMoveEvent(QMouseEvent *event) override {
            if((event->position().toPoint()-_press).manhattanLength()>=QApplication::startDragDistance()) _dragged=true;
            if(_dragged) QSplitterHandle::mouseMoveEvent(event);
        }
        void mouseReleaseEvent(QMouseEvent *event) override {
            QSplitterHandle::mouseReleaseEvent(event);
            if(_left && !_dragged && event->button()==Qt::LeftButton && clicked) clicked();
            _left=false;
        }
    private:
        QPoint _press; bool _dragged=false,_left=false;
    };
    QSplitterHandle *createHandle() override {
        auto *result=new Handle(orientation(),this);
        result->clicked=[this,result] { if(handle(2)==result && toggle_right) toggle_right(); };
        return result;
    }
};

static void trim_clipboard_connections() {
    const auto *mime=QApplication::clipboard()->mimeData();
    if(!mime) return;
    const auto bytes=mime->hasFormat("application/qt-nodes-graph")
        ? mime->data("application/qt-nodes-graph") : mime->text().toUtf8();
    auto document=QJsonDocument::fromJson(bytes).object();
    if(!document.value("nodes").isArray()) return;
    QSet<int> ids;
    for(const auto value:document.value("nodes").toArray()) ids.insert(value.toObject().value("id").toInt());
    QJsonArray connections;
    for(const auto value:document.value("connections").toArray()) {
        const auto edge=value.toObject();
        if(ids.contains(edge.value("outNodeId").toInt()) && ids.contains(edge.value("inNodeId").toInt()) && !connections.contains(value))
            connections.append(value);
    }
    // QtNodes Copy includes incident edges to unselected nodes. Its Paste then
    // remaps absent endpoints to ID 0, producing invalid links/failed insertion.
    if(connections==document.value("connections").toArray()) return;
    document["connections"]=connections;
    const auto corrected=QJsonDocument(document).toJson(QJsonDocument::Compact);
    auto *result=new QMimeData; result->setData("application/qt-nodes-graph",corrected);
    result->setText(QString::fromUtf8(corrected)); QApplication::clipboard()->setMimeData(result);
}

class LocalizedDataFlowScene final : public DataFlowGraphicsScene {
public:
    using DataFlowGraphicsScene::DataFlowGraphicsScene;
    std::function<void(int)> focus_graph;
    std::function<void(int,int)> open_graph;
    QMenu *createStdMenu(QPointF scene_position) {
        auto *menu=DataFlowGraphicsScene::createStdMenu(scene_position);
        for(auto *action:menu->actions()) {
            if(action->text()=="Copy") ui::bind(action,"text","Copy");
            else if(action->text()=="Cut") ui::bind(action,"text","Cut");
            connect(action,&QAction::triggered,this,[] { trim_clipboard_connections(); });
        }
        auto *paste=menu->addAction(ui::text("Paste")); paste->setShortcut(QKeySequence::Paste);
        connect(paste,&QAction::triggered,this,[this,scene_position] {
            trim_clipboard_connections();
            undoStack().push(new QtNodes::PasteCommand(this,scene_position));
        });
        for(auto *item:items(scene_position)) {
            QtNodes::NodeGraphicsObject *node=nullptr;
            for(auto *parent=item;parent;parent=parent->parentItem())
                if((node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(parent))) break;
            if(!node) continue;
            auto &graph=static_cast<DataFlowGraphModel &>(graphModel());
            auto *model=graph.delegateModel<AudioNodeModel>(node->nodeId());
            if(model && pipeline::node_definition(model->name()).category=="views") {
                menu->addSeparator(); const int id=int(node->nodeId());
                auto *focus=menu->addAction(ui::text("Focus graph")); focus->setObjectName("focusGraph");
                connect(focus,&QAction::triggered,this,[this,id] { if(focus_graph) focus_graph(id); });
                const std::array<const char *,3> labels{"Open in separate window","Open in new tab","Open beside current graph"};
                for(int placement=0;placement<3;++placement) {
                    auto *action=menu->addAction(ui::text(labels[placement]));
                    action->setObjectName(QStringLiteral("openGraphPlacement%1").arg(placement));
                    connect(action,&QAction::triggered,this,[this,id,placement] { if(open_graph) open_graph(id,placement); });
                }
            }
            break;
        }
        return menu;
    }
    QMenu *createSceneMenu(QPointF scene_position) override {
        if(!items(scene_position).empty()) return createStdMenu(scene_position);
        auto *menu=DataFlowGraphicsScene::createSceneMenu(scene_position);
        if(!menu) return nullptr;
        const QMap<QString,const char *> labels{{"inputs","Inputs"},{"input","Input"},
            {"transforms","Transforms"},{"transform","Transform"},{"views","Graphs"},
            {"graphs","Graphs"},{"outputs","Outputs"},{"output","Output"}};
        for(auto *tree:menu->findChildren<QTreeWidget *>()) {
            for(int i=0;i<tree->topLevelItemCount();++i) {
                auto *category=tree->topLevelItem(i);
                const auto stable=category->text(0);
                if(auto label=labels.value(stable.toLower(),nullptr)) {
                    category->setData(0,Qt::UserRole,stable);
                    category->setText(0,ui::text(label));
                }
            }
        }
        for(auto *field:menu->findChildren<QLineEdit *>())
            if(field->placeholderText()=="Filter") ui::bind(field,"placeholderText","Filter");
        // QtNodes creates nodes from leaf text; leave those stable model IDs
        // untouched. Only category headings and the search hint are localized.
        return menu;
    }
};

class GraphViewIndicator final : public QGraphicsItem {
public:
    enum { Type=QGraphicsItem::UserType+0x315 };
    explicit GraphViewIndicator(QGraphicsItem *parent) : QGraphicsItem(parent) {
        setAcceptedMouseButtons(Qt::NoButton);
    }
    int type() const override { return Type; }
    QRectF boundingRect() const override { return {0,0,16,16}; }
    void paint(QPainter *painter,const QStyleOptionGraphicsItem *,QWidget *) override {
        painter->setRenderHint(QPainter::Antialiasing);
        painter->setPen(QPen(QApplication::palette().color(QPalette::Highlight),1.5));
        painter->setBrush(Qt::NoBrush);
        painter->drawRoundedRect(QRectF(1,1,14,14),2,2);
        painter->drawPolyline(QPolygonF{{3,12},{6,8},{9,10},{13,4}});
    }
};

class ProfiledGraphicsView final : public GraphicsView {
  public:
    explicit ProfiledGraphicsView(QWidget *parent=nullptr) : GraphicsView(parent) {
        setMouseTracking(true); viewport()->setMouseTracking(true);
        _pan_timer.setInterval(16);
        _pan_timer.setTimerType(Qt::PreciseTimer);
        connect(&_pan_timer,&QTimer::timeout,this,[this] {
            if(!isVisible() || !hasFocus() || editing_control_focused() ||
               QApplication::keyboardModifiers()!=Qt::NoModifier) {
                stop_keyboard_pan(); return;
            }
            const double seconds=std::min(_pan_clock.nsecsElapsed()/1e9,0.05);
            _pan_clock.restart();
            QPointF direction;
            for(auto key:_held_pan_keys) direction+=pan_direction(key);
            const double length=std::hypot(direction.x(),direction.y());
            if(length>0) pan_pixels(direction*(360.0*(_navigation_speed/3.0)*seconds/length));
        });
        connect(qApp,&QApplication::focusChanged,this,[this](QWidget *,QWidget *) {
            if(!hasFocus() || editing_control_focused()) stop_keyboard_pan();
        });
    }
    std::function<void()> on_interaction;
    std::function<void()> synchronize_positions;
    std::function<void(QtNodes::NodeId,bool)> on_graph_clicked;
    void onCopySelectedObjects() override {
        if(editing_control_focused()) return;
        QtNodes::CopyCommand copy(nodeScene());
        trim_clipboard_connections();
    }
    void onPasteObjects() override {
        if(editing_control_focused() || !isInteractive()) return;
        const auto point=_pointer_seen && viewport()->rect().contains(_last_pointer)
            ? _last_pointer : viewport()->rect().center();
        trim_clipboard_connections();
        nodeScene()->undoStack().push(new QtNodes::PasteCommand(nodeScene(),mapToScene(point)));
    }
    void set_navigation_speed(float speed) {
        _navigation_speed=std::isfinite(speed) ? std::clamp(speed,0.5f,10.0f) : 3.0f;
    }
    void configure_graph_shortcuts() {
        for(auto *action:actions()) {
            const auto shortcuts=action->shortcuts();
            if(shortcuts.contains(QKeySequence(QKeySequence::Undo))) {
                action->setShortcuts({QKeySequence(Qt::ALT|Qt::Key_Z)});
            } else if(shortcuts.contains(QKeySequence(QKeySequence::Redo))) {
                action->setShortcuts({QKeySequence(Qt::ALT|Qt::SHIFT|Qt::Key_Z)});
            } else {
                QList<QKeySequence> remapped;
                for(const auto &shortcut:shortcuts) {
                    if(shortcut.count()==1 && shortcut[0].keyboardModifiers().testFlag(Qt::ControlModifier)) {
                        auto modifiers=shortcut[0].keyboardModifiers();
                        modifiers&=~Qt::ControlModifier; modifiers|=Qt::AltModifier;
                        remapped.append(QKeySequence(QKeyCombination(modifiers,shortcut[0].key())));
                    } else remapped.append(shortcut);
                }
                action->setShortcuts(remapped);
            }
            const QMap<QString,const char *> labels{{"Copy Selection","Copy selection"},{"Paste Selection","Paste selection"},
                {"Cut Selection","Cut selection"},{"Duplicate Selection","Duplicate selection"},{"Delete Selection","Delete selection"},
                {"&Undo","Undo"},{"&Redo","Redo"},{"Undo","Undo"},{"Redo","Redo"},{"Select All","Select all"}};
            const auto stable=action->text();
            if(auto label=labels.value(stable,nullptr)) {
                ui::bind(action,"text",label);
                if(stable=="Copy Selection") action->setShortcuts({QKeySequence(QKeySequence::Copy)});
                if(stable=="Paste Selection") action->setShortcuts({QKeySequence(QKeySequence::Paste)});
                if(stable=="Cut Selection") action->setShortcuts({QKeySequence(QKeySequence::Cut)});
            }
        }
    }
    void remember_camera() {
        if (!_restoring && isVisible()) _center=mapToScene(viewport()->rect().center());
    }
    QPointF camera_center() const { return _center; }
    void camera(double scale, QPointF center) {
        _restoring = true;
        // setupScale skips equal scales; reset also removes old zoom translations.
        QTransform transform; transform.scale(scale,scale); setTransform(transform);
        centerOn(center); _center = center;
        _restoring = false;
        emit scaleChanged(getScale());
    }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override {
        QGraphicsView::contextMenuEvent(event);
    }
    bool viewportEvent(QEvent *event) override {
        if(event->type()!=QEvent::ContextMenu) return GraphicsView::viewportEvent(event);
        auto *context=static_cast<QContextMenuEvent *>(event);
        for(auto *item:items(context->pos())) if(auto *proxy=qgraphicsitem_cast<QGraphicsProxyWidget *>(item)) {
            auto *child=proxy->widget() ? proxy->widget()->childAt(proxy->mapFromScene(mapToScene(context->pos())).toPoint()) : nullptr;
            if(is_editing_widget(child)) return GraphicsView::viewportEvent(event);
        }
        show_context_menu(context);
        return true;
    }
    void show_context_menu(QContextMenuEvent *event) {
        _last_pointer=event->pos(); _pointer_seen=true;
        auto *scene=static_cast<LocalizedDataFlowScene *>(nodeScene());
        if(!scene) return;
        auto *menu=items(event->pos()).empty() ? scene->createSceneMenu(mapToScene(event->pos()))
                                             : scene->createStdMenu(mapToScene(event->pos()));
        if(menu) menu->exec(event->globalPos());
        event->accept();
    }
    bool event(QEvent *event) override {
        if(event->type()==QEvent::ShortcutOverride && editing_control_focused()) {
            const QKeySequence key(static_cast<QKeyEvent *>(event)->keyCombination());
            for(auto *action:actions()) if(action->shortcuts().contains(key)) {
                event->accept(); return true;
            }
        }
        return GraphicsView::event(event);
    }
    void keyPressEvent(QKeyEvent *event) override {
        if(event->modifiers()!=Qt::NoModifier || editing_control_focused()) {
            stop_keyboard_pan();
            GraphicsView::keyPressEvent(event); return;
        }
        const int key=physical_pan_key(event);
        if(!key) { GraphicsView::keyPressEvent(event); return; }
        if(!event->isAutoRepeat()) {
            _held_pan_keys.insert(key);
            if(!_pan_timer.isActive()) { _pan_clock.start(); _pan_timer.start(); }
        }
        event->accept();
    }
    void keyReleaseEvent(QKeyEvent *event) override {
        const int key=physical_pan_key(event);
        if(!key) { GraphicsView::keyReleaseEvent(event); return; }
        if(!_held_pan_keys.contains(key)) { GraphicsView::keyReleaseEvent(event); return; }
        if(!event->isAutoRepeat()) {
            _held_pan_keys.remove(key);
            if(_held_pan_keys.isEmpty()) stop_keyboard_pan();
        }
        event->accept();
    }
    void focusOutEvent(QFocusEvent *event) override {
        stop_keyboard_pan(); finish_middle_pan();
        GraphicsView::focusOutEvent(event);
    }
    static int physical_pan_key(const QKeyEvent *event) {
#ifdef Q_OS_WIN
        // Set-1 physical scan codes, independent of the active Windows layout.
        switch(event->nativeScanCode()) {
        case 0x11: return Qt::Key_W;
        case 0x1e: return Qt::Key_A;
        case 0x1f: return Qt::Key_S;
        case 0x20: return Qt::Key_D;
        }
#endif
        if(event->nativeScanCode()==0) switch(event->key()) {
        case Qt::Key_W: case Qt::Key_A: case Qt::Key_S: case Qt::Key_D: return event->key();
        }
        return 0;
    }
    void mousePressEvent(QMouseEvent *event) override {
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        _press_pointer=_last_pointer; _pressed_node=QtNodes::InvalidNodeId;
        _press_alt=event->modifiers().testFlag(Qt::AltModifier);
        if(event->button()==Qt::LeftButton)
            for(auto *item:items(_last_pointer)) {
                for(auto *parent=item;parent;parent=parent->parentItem())
                    if(auto *node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(parent)) {
                        _pressed_node=node->nodeId(); break;
                    }
                if(_pressed_node!=QtNodes::InvalidNodeId) break;
            }
        stop_keyboard_pan();
        if(event->button()==Qt::MiddleButton && event->buttons()==Qt::MiddleButton &&
           event->modifiers()==Qt::NoModifier && !editing_control_focused()) {
            _middle_panning=true;
            _middle_last=event->position().toPoint();
            _previous_cursor=viewport()->cursor();
            viewport()->setCursor(Qt::ClosedHandCursor);
            setFocus(Qt::MouseFocusReason);
            event->accept(); return;
        }
        GraphicsView::mousePressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        if(_middle_panning) {
            if(!(event->buttons() & Qt::MiddleButton)) {
                finish_middle_pan(); GraphicsView::mouseMoveEvent(event); return;
            }
            const auto point=event->position().toPoint();
            pan_pixels(_middle_last-point); _middle_last=point;
            event->accept(); return;
        }
        GraphicsView::mouseMoveEvent(event);
    }
    void drawBackground(QPainter *painter, const QRectF &rect) override {
        // Use the current palette instead of the brush cached by QtNodes at
        // construction. Sparse cosmetic lines stay subdued at every zoom.
        const auto palette = QApplication::palette();
        const auto background = palette.color(QPalette::Window);
        const auto foreground = palette.color(QPalette::WindowText);
        painter->fillRect(rect, background);
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        double step = 60;
        while (step * getScale() < 32) step *= 2;
        auto grid = [&](double spacing, double amount) {
            const auto color = QColor::fromRgbF(
                background.redF() * (1-amount) + foreground.redF() * amount,
                background.greenF() * (1-amount) + foreground.greenF() * amount,
                background.blueF() * (1-amount) + foreground.blueF() * amount);
            painter->setPen(QPen(color, 0));
            for (double x = std::floor(rect.left()/spacing)*spacing; x <= rect.right(); x += spacing)
                painter->drawLine(QLineF(x, rect.top(), x, rect.bottom()));
            for (double y = std::floor(rect.top()/spacing)*spacing; y <= rect.bottom(); y += spacing)
                painter->drawLine(QLineF(rect.left(), y, rect.right(), y));
        };
        grid(step, 0.06);
        grid(step*5, 0.10);
        painter->restore();
    }
    void showEvent(QShowEvent *event) override {
        _restoring = true;
        QGraphicsView::showEvent(event); // QtNodes centers the document on every show.
        centerOn(_center); _restoring = false;
    }
    void hideEvent(QHideEvent *event) override {
        stop_keyboard_pan();
        finish_middle_pan();
        // Also catch explicit QGraphicsView::centerOn callers before visibility
        // changes. Ignore scrollbar pixel rounding so repeated docking cannot drift.
        const auto actual=mapToScene(viewport()->rect().center());
        if (viewport()->width()>0 && viewport()->height()>0 && QLineF(actual,_center).length()>3/getScale())
            _center=actual;
        QGraphicsView::hideEvent(event);
    }
    void resizeEvent(QResizeEvent *event) override {
        _restoring = true;
        QGraphicsView::resizeEvent(event);
        centerOn(_center); _restoring = false;
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        if(_middle_panning) {
            if(event->button()==Qt::MiddleButton) finish_middle_pan();
            event->accept(); return;
        }
        GraphicsView::mouseReleaseEvent(event);
        if(event->button()==Qt::LeftButton && !_press_alt && _pressed_node!=QtNodes::InvalidNodeId && !editing_control_focused() &&
           (_last_pointer-_press_pointer).manhattanLength()<QApplication::startDragDistance() && on_graph_clicked)
            on_graph_clicked(_pressed_node,_press_alt);
        _pressed_node=QtNodes::InvalidNodeId;
        remember_camera();
        if (synchronize_positions) synchronize_positions();
        if (on_interaction) on_interaction();
    }
    void wheelEvent(QWheelEvent *event) override {
        const double delta=event->pixelDelta().isNull() ? event->angleDelta().y()/120.0 : event->pixelDelta().y()/40.0;
        if (delta==0) { event->ignore(); return; }
        const QPointF anchor=mapToScene(event->position().toPoint());
        const double next=std::clamp(getScale()*std::exp(std::clamp(delta,-10.0,10.0)*0.10),0.2,3.0);
        const auto offset = QPointF(event->position().toPoint()-viewport()->rect().center())/next;
        camera(next,anchor-offset);
        event->accept(); if (on_interaction) on_interaction();
    }
    void paintEvent(QPaintEvent *event) override {
        core::perf::Scope trace(core::perf::Stage::EditorPaint);
        GraphicsView::paintEvent(event);
    }
  private:
    QPointF _center;
    bool _restoring = false;
    bool _middle_panning = false;
    QPoint _middle_last;
    QCursor _previous_cursor;
    QPoint _last_pointer, _press_pointer;
    bool _pointer_seen=false, _press_alt=false;
    QtNodes::NodeId _pressed_node=QtNodes::InvalidNodeId;
    QTimer _pan_timer;
    QElapsedTimer _pan_clock;
    QSet<int> _held_pan_keys;
    float _navigation_speed=3.0f;
    static QPointF pan_direction(int key) {
        switch(key) {
        case Qt::Key_W: return {0,-1};
        case Qt::Key_A: return {-1,0};
        case Qt::Key_S: return {0,1};
        case Qt::Key_D: return {1,0};
        default: return {};
        }
    }
    void stop_keyboard_pan() { _pan_timer.stop(); _held_pan_keys.clear(); }
    static bool is_editing_widget(QWidget *widget) {
            for(auto *current=widget;current;current=current->parentWidget())
                if(qobject_cast<QLineEdit *>(current) || qobject_cast<QAbstractSpinBox *>(current) ||
                   qobject_cast<QComboBox *>(current) || qobject_cast<QTextEdit *>(current) ||
                   qobject_cast<QPlainTextEdit *>(current)) return true;
            return false;
    }
    bool editing_control_focused() const {
        if(is_editing_widget(QApplication::focusWidget())) return true;
        if(scene()) if(auto *proxy=qgraphicsitem_cast<QGraphicsProxyWidget *>(scene()->focusItem()))
            if(proxy->widget() && is_editing_widget(proxy->widget()->focusWidget())) return true;
        return false;
    }
    void pan_pixels(QPointF delta) {
        _restoring=true;
        _center+=delta/getScale();
        centerOn(_center);
        _restoring=false;
        if(on_interaction) on_interaction();
    }
    void finish_middle_pan() {
        if(!_middle_panning) return;
        _middle_panning=false;
        viewport()->setCursor(_previous_cursor);
    }
};

static void setStyle__() {
    const auto palette=QApplication::palette();
    const auto background=palette.color(QPalette::Window);
    const auto foreground=palette.color(QPalette::WindowText);
    auto grid_color=[&](double amount) {
        return QColor::fromRgbF(background.redF()*(1-amount)+foreground.redF()*amount,
                                background.greenF()*(1-amount)+foreground.greenF()*amount,
                                background.blueF()*(1-amount)+foreground.blueF()*amount);
    };
    auto view=QtNodes::StyleCollection::flowViewStyle();
    view.BackgroundColor=background;
    view.FineGridColor=grid_color(0.06);
    view.CoarseGridColor=grid_color(0.10);
    QtNodes::StyleCollection::setGraphicsViewStyle(view);
    auto json=QtNodes::StyleCollection::connectionStyle().toJson();
    auto connection=json["ConnectionStyle"].toObject();
    connection["ConstructionColor"]=palette.color(QPalette::Mid).name();
    connection["NormalColor"]=palette.color(QPalette::Text).name();
    connection["SelectedColor"]=palette.color(QPalette::Highlight).name();
    connection["SelectedHaloColor"]=palette.color(QPalette::Highlight).name();
    connection["HoveredColor"]=palette.color(QPalette::Highlight).name();
    connection["UseDataDefinedColors"]=true;
    json["ConnectionStyle"]=connection;
    ConnectionStyle::setConnectionStyle(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

static QJsonObject processing_document(const QJsonObject &document) {
    QJsonArray nodes, connections;
    std::vector<QJsonObject> sorted;
    for (const auto value : document.value("nodes").toArray()) {
        const auto node=value.toObject();
        const auto internal=node.value("internal-data").toObject();
        const auto kind=pipeline::canonical_node_name(internal.value("model-name").toString());
        auto parameters=pipeline::node_definition(kind).defaults;
        for(auto it=internal.begin();it!=internal.end();++it) parameters[it.key()]=it.value();
        parameters["model-name"]=kind;
        sorted.push_back({{"id",node.value("id")},{"internal-data",parameters}});
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) { return a.value("id").toInt() < b.value("id").toInt(); });
    for (const auto &node : sorted) nodes.append(node);
    sorted.clear();
    for (const auto value : document.value("connections").toArray()) sorted.push_back(QtNodes::toJson(QtNodes::fromJson(value.toObject())));
    std::sort(sorted.begin(), sorted.end(), [](const auto &a, const auto &b) {
        return QJsonDocument(a).toJson(QJsonDocument::Compact) < QJsonDocument(b).toJson(QJsonDocument::Compact);
    });
    for (const auto &edge : sorted) connections.append(edge);
    return {{"nodes",nodes},{"connections",connections}};
}
void SchemeEditorWidget::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if(event->type()==QEvent::PaletteChange && _view && _scene) {
        setStyle__();
        _view->resetCachedContent();
        _scene->invalidate();
        _view->viewport()->update();
    }
}
static QJsonObject editable_document(const QJsonObject &document, bool &unsupported) {
    auto visible=document; QJsonArray nodes,connections; QSet<int> ids;
    const auto version=document.value("version").toInt();
    unsupported=!document.isEmpty() && version!=1 && version!=2;
    for(const auto &value:document.value("nodes").toArray()) {
        const auto node=value.toObject();
        if(pipeline::node_definition(node.value("internal-data").toObject().value("model-name").toString()).name.isEmpty()) {
            unsupported=true; continue;
        }
        nodes.append(node); ids.insert(node.value("id").toInt());
    }
    for(const auto &value:document.value("connections").toArray()) {
        const auto edge=value.toObject();
        if(ids.contains(edge.value("outNodeId").toInt()) && ids.contains(edge.value("inNodeId").toInt())) connections.append(edge);
    }
    visible["nodes"]=nodes; visible["connections"]=connections; return visible;
}
class ArrangeCommand final : public QUndoCommand {
    DataFlowGraphModel &_graph;
    std::map<QtNodes::NodeId,QPointF> _before, _after;
    void set(const auto &positions) {
        for (const auto &[id, position] : positions)
            if (_graph.nodeExists(id)) _graph.setNodeData(id, QtNodes::NodeRole::Position, position);
    }
  public:
    ArrangeCommand(DataFlowGraphModel &graph, std::map<QtNodes::NodeId,QPointF> positions, const QString &label = {})
        : _graph(graph), _after(std::move(positions)) {
        setText(label.isEmpty() ? ui::text("Arrange Data Flow") : label);
        for (const auto &[id, position] : _after) _before[id] = graph.nodeData(id, QtNodes::NodeRole::Position).toPointF();
    }
    void redo() override { set(_after); }
    void undo() override { set(_before); }
};
class TestSourceCommand final : public QUndoCommand {
    DataFlowGraphModel &_graph;
    QJsonObject _before, _after;
    std::vector<QtNodes::ConnectionId> _connections;
    void replace(const QJsonObject &node) {
        const auto id = node.value("id").toInt();
        if (_graph.nodeExists(id)) _graph.deleteNode(id);
        _graph.loadNode(node);
        for (const auto &connection : _connections)
            if (_graph.connectionPossible(connection)) _graph.addConnection(connection);
    }
public:
    TestSourceCommand(DataFlowGraphModel &graph, QJsonObject node, const QJsonArray &connections)
        : _graph(graph), _before(node), _after(std::move(node)) {
        setText("Use silent test source");
        auto params = pipeline::TestSignalSettings{}.json(); params["model-name"] = "TestSignal";
        _after["internal-data"] = params;
        const auto id = _before.value("id").toInt();
        for (const auto &value : connections) {
            const auto connection = QtNodes::fromJson(value.toObject());
            if (connection.outNodeId == id || connection.inNodeId == id) _connections.push_back(connection);
        }
    }
    void redo() override { replace(_after); }
    void undo() override { replace(_before); }
};

SchemeEditorWidget::SchemeEditorWidget(GuiPresenter &application, QWidget *parent)
    : QWidget(parent), _application(application),
      _scene(new LocalizedDataFlowScene(*application.state().data_flow_graph, this)),
      _view(new ProfiledGraphicsView(this)) {
    setObjectName("pipelineEditor");
    _document_template = pipeline::canonical_graph_document(application.state().file_configs.scene_graph);
    editable_document(_document_template,_unsupported_document);
    retain_unrendered_connections();
    setStyle__();
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this] {
        setStyle__();
        _view->setBackgroundBrush(QApplication::palette().color(QPalette::Window));
        _view->resetCachedContent();
        _scene->invalidate();_view->viewport()->update();
    });
    _scene->setGroupingEnabled(false);
    _view->setScene(_scene);
    static_cast<ProfiledGraphicsView *>(_view)->configure_graph_shortcuts();
    _view->setScaleRange(0.2,3.0);
    _view->setTransformationAnchor(QGraphicsView::NoAnchor);
    _view->setResizeAnchor(QGraphicsView::NoAnchor);
    _view->setSceneRect(-10000000,-10000000,20000000,20000000);
    _view->setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    _view->setInteractive(!_unsupported_document);
    auto *layout = new QVBoxLayout(this);
    auto *bar = new QHBoxLayout;
    bar->setSpacing(6);
    auto *apply_button = ui::make<QPushButton>("Apply", this);
    _apply_button=apply_button;
    apply_button->setObjectName("applyPipeline");
    auto *presets=new QToolButton(this); presets->setObjectName("pipelinePresets");
    _presets_button=presets; presets->hide();
    ui::bind(presets,"text","Presets"); presets->setPopupMode(QToolButton::InstantPopup);
    presets->setStyleSheet("QToolButton::menu-indicator { image: none; width: 0px; }");
    auto *preset_menu=new QMenu(presets); preset_menu->setObjectName("pipelinePresetsMenu"); presets->setMenu(preset_menu);
    ui::bind(preset_menu,"title","Presets");
    auto action=[preset_menu](const char *text,const char *id) {
        auto *item=preset_menu->addAction(ui::text(text)); item->setObjectName(id); ui::bind(item,"text",text); return item;
    };
    auto *reset=action("Default","defaultPipeline");
    auto *load_active=action("Applied Data Flow","loadActivePipeline");
    auto *test_source=action("Generator example","useTestSource");
    preset_menu->addSeparator();
    auto *user_presets=preset_menu->addMenu(ui::text("My presets")); ui::bind(user_presets,"title","My presets");
    auto *save=action("Save preset…","savePipelinePreset");
    auto *delete_presets=preset_menu->addMenu(ui::text("Delete preset")); ui::bind(delete_presets,"title","Delete preset");
    auto refresh_presets=[this,user_presets,delete_presets] {
        user_presets->clear(); delete_presets->clear();
        for (const auto &value : _application.state().file_configs.ui.pipeline_presets) {
            const auto preset=value.toObject(); const auto id=preset.value("id").toString();
            if (id.isEmpty() || !preset.value("graph").isObject()) continue;
            const auto name=preset.value("name").toString();
            auto *load=user_presets->addAction(name);
            connect(load,&QAction::triggered,this,[this,id] { load_preset(id); });
            auto *remove=delete_presets->addAction(name);
            connect(remove,&QAction::triggered,this,[this,id] { remove_preset(id); });
        }
        user_presets->setEnabled(!user_presets->isEmpty()); delete_presets->setEnabled(!delete_presets->isEmpty());
    };
    refresh_presets(); connect(preset_menu,&QMenu::aboutToShow,this,refresh_presets);
    connect(save,&QAction::triggered,this,[this] {
        bool ok=false; const auto name=QInputDialog::getText(this,ui::text("Save preset"),ui::text("Preset name"),QLineEdit::Normal,QString(),&ok);
        if (ok && !name.trimmed().isEmpty()) save_preset(name);
    });
    bar->addWidget(apply_button);
    connect(test_source,&QAction::triggered,this,[this] {
        auto &graph = *_application.state().data_flow_graph;
        const auto document = graph.save();
        for (const auto &value : document.value("nodes").toArray()) {
            const auto node = value.toObject();
            if (node.value("internal-data").toObject().value("model-name") == "AudioSource") {
                _scene->undoStack().push(new TestSourceCommand(graph,node,document.value("connections").toArray()));
                return;
            }
        }
        const auto nodes=document.value("nodes").toArray();
        if (std::any_of(nodes.begin(),nodes.end(),[](const QJsonValue &value) {
            return value.toObject().value("internal-data").toObject().value("model-name")=="TestSignal";
        })) return;
        set_status(ui::text("No audio source in this draft"));
    });
    auto *execution=bar;
    execution->addStretch();
    auto *zoom=new QDoubleSpinBox(this); zoom->setObjectName("pipelineZoom");
    zoom->setMaximumWidth(85);
    zoom->setDecimals(0); zoom->setRange(20,300); zoom->setSuffix(" %"); zoom->setSingleStep(10); zoom->setValue(100);
    execution->addWidget(ui::make<QLabel>("Zoom",this)); execution->addWidget(zoom);
    connect(zoom,&QDoubleSpinBox::valueChanged,this,[this](double percent) {
        const auto center=_view->mapToScene(_view->viewport()->rect().center());
        static_cast<ProfiledGraphicsView *>(_view)->camera(percent/100,center);
    });
    connect(_view,&GraphicsView::scaleChanged,zoom,[zoom](double value) {
        const QSignalBlocker blocker(zoom); zoom->setValue(value*100);
    });
    auto *fit=ui::make<QPushButton>("Center",this); fit->setObjectName("fitPipeline");
    ui::bind(fit,"toolTip","Fit the Data Flow in view (Alt+F)");
    execution->insertWidget(2,fit);
    connect(fit,&QPushButton::clicked,this,[this] {
        fit_nodes();
    });
    layout->addLayout(bar);
    auto *splitter = new InspectorSplitter(Qt::Horizontal,this);
    _splitter=splitter; splitter->setObjectName("pipelineSplitter");
    auto *library = new QTreeWidget(splitter); library->setObjectName("pipelineNodeLibrary");
    library->setBackgroundRole(QPalette::Window);
    library->viewport()->setBackgroundRole(QPalette::Window);
    library->viewport()->setAutoFillBackground(true);
    library->setHeaderHidden(true); library->setMinimumWidth(140);
    QMap<QString,QTreeWidgetItem *> categories;
    const QMap<QString,const char *> labels{{"inputs","Inputs"},{"transforms","Transforms"},{"views","Graphs"},{"outputs","Outputs"}};
    for (const auto &definition : pipeline::node_definitions()) {
        if (!categories.contains(definition.category)) {
            auto *category = new QTreeWidgetItem(library);
            category->setText(0,ui::text(labels.value(definition.category,"Nodes")));
            categories[definition.category] = category;
        }
        auto *item = new QTreeWidgetItem(categories[definition.category]);
        AudioNodeModel delegate(definition.name); item->setText(0,delegate.caption());
        item->setData(0,Qt::UserRole,definition.name);
    }
    library->expandAll();
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this,categories,labels] {
        for(auto it=categories.begin();it!=categories.end();++it) {
            it.value()->setText(0,ui::text(labels.value(it.key(),"Nodes")));
            for(int i=0;i<it.value()->childCount();++i) {
                auto *item=it.value()->child(i);
                AudioNodeModel delegate(item->data(0,Qt::UserRole).toString());
                item->setText(0,delegate.caption());
            }
        }
        if(auto *title=findChild<QLabel *>("pipelineInspectorTitle"))
            if(auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(_inspected_node))
                title->setText(model->caption());
    });
    library->setEnabled(!_unsupported_document);
    connect(library,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem *item) {
        const auto kind=item->data(0,Qt::UserRole).toString();
        if (!kind.isEmpty()) _scene->undoStack().push(new QtNodes::CreateCommand(_scene,kind,
            _view->mapToScene(_view->viewport()->rect().center())));
    });
    splitter->addWidget(_view);
    auto *inspector = new QScrollArea(splitter); inspector->setObjectName("pipelineInspector");
    _inspector=inspector;
    inspector->setWidgetResizable(true);
    inspector->setFrameShape(QFrame::NoFrame);
    inspector->setBackgroundRole(QPalette::Window);
    inspector->viewport()->setBackgroundRole(QPalette::Window);
    inspector->setAutoFillBackground(true);
    auto *contents=new QWidget(inspector); _inspector_layout=new QVBoxLayout(contents);
    inspector->setWidget(contents); inspect_node(QtNodes::InvalidNodeId);
    splitter->setCollapsible(0,false); splitter->setCollapsible(1,false); splitter->setCollapsible(2,true);
    splitter->setStretchFactor(1,1); splitter->setSizes({170,750,280});
    ui::bind(splitter->handle(2),"toolTip","Drag to resize; click to collapse or expand parameters");
    splitter->toggle_right=[this] { set_inspector_expanded(_splitter->sizes().value(2)==0); };
    layout->addWidget(splitter, 1);
    connect(_scene,&DataFlowGraphicsScene::nodeSelected,this,&SchemeEditorWidget::inspect_node);
    connect(_scene,&DataFlowGraphicsScene::nodeDoubleClicked,this,[this](QtNodes::NodeId id) {
        auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
        if(model && pipeline::node_definition(model->name()).category=="views")
            emit graph_view_requested(int(id));
    });
    auto request_graph=[this](QtNodes::NodeId id,bool) {
        auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
        if(!model || pipeline::node_definition(model->name()).category!="views") return;
        emit graph_view_requested(int(id));
    };
    static_cast<ProfiledGraphicsView *>(_view)->on_graph_clicked=request_graph;
    auto *localized_scene=static_cast<LocalizedDataFlowScene *>(_scene);
    localized_scene->focus_graph=[request_graph](int id) { request_graph(QtNodes::NodeId(id),false); };
    localized_scene->open_graph=[this](int id,int placement) { emit graph_open_requested(id,placement); };
    connect(_scene,&QGraphicsScene::selectionChanged,this,[this] {
        if (_scene->selectedItems().isEmpty()) inspect_node(QtNodes::InvalidNodeId);
    });
    _status = new QLabel(this);
    _status->setObjectName("pipelineValidation");
    _status->setFixedSize(24,24);
    _status->setAlignment(Qt::AlignCenter);
    auto update_status_icon=[this] {
        _status->setPixmap(_status->style()->standardIcon(QStyle::SP_MessageBoxWarning).pixmap(16,16));
    };
    update_status_icon();
    connect(&ui::Theme::instance(),&ui::Theme::changed,_status,update_status_icon);
    _status->hide();
    bar->addWidget(_status);
    connect(this,&SchemeEditorWidget::status_message,apply_button,&QPushButton::setToolTip);
    _apply_timer.setSingleShot(true);
    _apply_timer.setInterval(350);
    connect(&_apply_timer, &QTimer::timeout, this, [this] {
        if (_application.state().file_configs.ui.pipeline_auto_apply)
            apply();
        else if (_application.state().file_configs.ui.autosave_enabled)
            emit _application.on_config_updated();
    });
    connect(apply_button, &QPushButton::clicked, this, &SchemeEditorWidget::apply);
    connect(&application,&GuiPresenter::on_config_updated,this,&SchemeEditorWidget::sync_preferences);
    _auto_apply=application.state().file_configs.ui.pipeline_auto_apply;
    sync_preferences();
    connect(load_active, &QAction::triggered, this, &SchemeEditorWidget::load_active_pipeline);
    _last_processing_document = processing_document(application.state().data_flow_graph->save());
    _save_timer.setSingleShot(true); _save_timer.setInterval(350);
    connect(&_save_timer, &QTimer::timeout, this, [this] {
        save_view();
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    });
    auto &model = *application.state().data_flow_graph;
    for(auto id:model.allNodeIds()) wire_source_model(id);
    connect(&model,&DataFlowGraphModel::nodeCreated,this,&SchemeEditorWidget::wire_source_model);
    connect(&model,&DataFlowGraphModel::nodeCreated,this,&SchemeEditorWidget::update_graph_view_indicator);
    connect(&model,&DataFlowGraphModel::nodeUpdated,this,&SchemeEditorWidget::update_graph_view_indicator);
    for(auto id:model.allNodeIds()) update_graph_view_indicator(id);
    auto refresh_indicators=[this] {
        for(auto id:_application.state().data_flow_graph->allNodeIds()) update_graph_view_indicator(id);
    };
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,refresh_indicators);
    connect(&ui::Language::instance(),&ui::Language::changed,this,refresh_indicators);
    connect(&model,&DataFlowGraphModel::nodeDeleted,this,[this](QtNodes::NodeId id) { _source_start_intents.remove(id); });
    connect(_scene, &DataFlowGraphicsScene::modified, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodePositionUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeDeleted, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionDeleted, this, &SchemeEditorWidget::changed);
    connect(&_scene->undoStack(), &QUndoStack::indexChanged, this, &SchemeEditorWidget::changed);
    auto *arrange = ui::make<QPushButton>("Arrange", this);
    ui::bind(arrange,"toolTip","Arrange nodes from left to right (Alt+A)");
    arrange->setObjectName("arrangePipeline"); bar->insertWidget(2,arrange);
    connect(arrange, &QPushButton::clicked, this, &SchemeEditorWidget::arrange_nodes);
    auto scoped_shortcut=[this](const char *label,const char *id,QKeySequence key,auto handler) {
        // Alt+A previously belonged to QtNodes Select All. The requested
        // Arrange shortcut takes precedence inside the canvas only.
        for(auto *existing:_view->actions()) {
            auto shortcuts=existing->shortcuts();
            shortcuts.removeAll(key); existing->setShortcuts(shortcuts);
        }
        auto *action=new QAction(_view); action->setObjectName(id);
        ui::bind(action,"text",label); action->setShortcut(key);
        action->setShortcutContext(Qt::WidgetShortcut); action->setAutoRepeat(false);
        _view->addAction(action); connect(action,&QAction::triggered,this,handler);
        return action;
    };
    _arrange_action=scoped_shortcut("Arrange Data Flow","arrangePipelineShortcut",QKeySequence(Qt::ALT|Qt::Key_A),&SchemeEditorWidget::arrange_nodes);
    _center_action=scoped_shortcut("Center Data Flow","fitPipelineShortcut",QKeySequence(Qt::ALT|Qt::Key_F),&SchemeEditorWidget::fit_nodes);
    connect(_view, &GraphicsView::scaleChanged, this, [this] { if (!_loading) _save_timer.start(); });
    for (auto *scroll : {_view->horizontalScrollBar(), _view->verticalScrollBar()})
        connect(scroll, &QScrollBar::sliderMoved, this, [this] { if (!_loading) _save_timer.start(); });
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = [this] { if (!_loading) _save_timer.start(); };
    static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions = [this] {
        if (_loading) return;
        auto &graph = *_application.state().data_flow_graph;
        std::map<QtNodes::NodeId,QPointF> moved;
        for (const auto id : graph.allNodeIds()) {
            auto *item = _scene->nodeGraphicsObject(id);
            if (item && item->pos() != graph.nodeData(id, QtNodes::NodeRole::Position).toPointF())
                moved[id] = item->pos();
        }
        if (!moved.empty()) _scene->undoStack().push(new ArrangeCommand(graph, std::move(moved), ui::text("Move nodes")));
    };
    connect(reset, &QAction::triggered, this, [this] {
        if (_application.graph_request_pending()) {
            set_status(ui::text("Wait for the pending graph command to finish before loading the active Data Flow."));
            return;
        }
        load_document(pipeline::GraphProgram::defaults(_application.state().file_configs.core.spectrum_channel,
                                                    _application.state().file_configs.core.spectrum_merge,
                                                    _application.state().file_configs.core.capture_source));
        _default_start_intent = true;
        if (_application.state().file_configs.ui.pipeline_auto_apply) apply();
        else set_status({});
    });
    connect(&_application, &GuiPresenter::pipeline_preview, this, [this](const auto &previews) {
        if (!isVisible() || (_preview_clock.isValid() && _preview_clock.elapsed() < 100))
            return;
        _preview_clock.start();
        if (_unsupported_document || processing_document(_application.state().data_flow_graph->save()) !=
            processing_document(_application.client().snapshot().graph)) return;
        for (auto it = previews.begin(); it != previews.end(); ++it)
            if (auto *model = _application.state().data_flow_graph->delegateModel<AudioNodeModel>(it.key()))
                model->preview(it.value());
    });
    connect(&application,&GuiPresenter::apply_status_changed,this,[this](const QString &status) {
        if(status.startsWith("Error: ")) set_status(ui::text("Not applied: ")+status.mid(7));
        else if(status.startsWith(ui::text("Not applied: "))) set_status(status);
    });
    connect(&application,&GuiPresenter::selected_device_changed,this,[this](const QJsonObject &draft,const QJsonObject &view) {
        load_document(draft,view,false,true);
    });
    connect(&application,&GuiPresenter::views_cleared,this,[this] {
        for (const auto id : _application.state().data_flow_graph->allNodeIds())
            if (auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) model->preview({});
    });
    connect(&application, &GuiPresenter::graph_apply_finished, this,
            [this](quint64 request, bool accepted, const QString &error) {
                if (request != _apply_request)
                    return;
                _apply_request = 0;
                if (accepted) cancel_start_intent();
                set_status(accepted ? QString{} : ui::text("Not applied: ") + error);
            });
    connect(&application, &GuiPresenter::saved_graph_warning_changed, this,
            [this](const QString &warning) { if (!warning.isEmpty()) set_status(warning); });
    QString error;
    auto draft = document();
    set_status(pipeline::GraphProgram::compile(draft, error)
                         ? QString{}
                         : ui::text("Draft not active: ") + error);
    const auto generation = _document_generation;
    QTimer::singleShot(0, this, [this,generation] {
        if (generation != _document_generation) return;
        _loading = true;
        const auto saved = _application.state().file_configs.ui.editor_view;
        if (_document_template.value("layout_pending").toBool()) {
            layout_nodes(false);
            _document_template.remove("layout_pending");
            _application.state().editor_draft_modified=true;
            _save_timer.start();
        }
        if (saved.value("version").toInt() == 1) {
            const auto scale = saved.value("scale").toDouble(1);
            const auto x = saved.value("center_x").toDouble(), y = saved.value("center_y").toDouble();
            if (std::isfinite(scale) && std::isfinite(x) && std::isfinite(y)) {
                static_cast<ProfiledGraphicsView *>(_view)->camera(std::clamp(scale,0.2,3.0),{x,y});
            } else static_cast<ProfiledGraphicsView *>(_view)->camera(1,node_bounds().center());
        } else fit_nodes();
        _loading = false;
    });
}
void SchemeEditorWidget::set_status(const QString &text) {
    _status->setToolTip(text);
    _status->setAccessibleName(text);
    _status->setAccessibleDescription(text);
    _status->setVisible(!text.isEmpty());
    emit status_message(text);
}
void SchemeEditorWidget::sync_preferences() {
    const bool automatic=_application.state().file_configs.ui.pipeline_auto_apply;
    _apply_button->setVisible(!automatic);
    if(automatic!=_auto_apply) {
        _auto_apply=automatic;
        _apply_timer.stop();
        QString error;
        if(automatic && !_loading && !_unsupported_document &&
           pipeline::GraphProgram::compile(document(),error) &&
           (_default_start_intent || !_source_start_intents.isEmpty() ||
            processing_document(document())!=processing_document(_application.client().snapshot().graph)))
            _apply_timer.start();
    }
}
void SchemeEditorWidget::set_navigation_speed(float speed) {
    static_cast<ProfiledGraphicsView *>(_view)->set_navigation_speed(speed);
}
QJsonObject SchemeEditorWidget::document() const {
    auto saved=_document_template;
    const auto graph=_application.state().data_flow_graph->save();
    for(auto it=graph.begin();it!=graph.end();++it) saved[it.key()]=it.value();
    QMap<int,QJsonObject> originals;
    for(const auto &value:_document_template.value("nodes").toArray()) {
        const auto node=value.toObject(); originals[node.value("id").toInt()]=node;
    }
    QJsonArray nodes;
    QSet<int> unavailable;
    for(const auto &value:_document_template.value("nodes").toArray()) {
        const auto node=value.toObject();
        if(pipeline::node_definition(node.value("internal-data").toObject().value("model-name").toString()).name.isEmpty()) {
            nodes.append(node); unavailable.insert(node.value("id").toInt());
        }
    }
    for(const auto &value:graph.value("nodes").toArray()) {
        const auto edited=value.toObject(); auto node=originals.value(edited.value("id").toInt());
        for(auto it=edited.begin();it!=edited.end();++it) node[it.key()]=it.value();
        nodes.append(node);
    }
    saved["nodes"]=nodes;
    QJsonArray connections;
    for(const auto &value:graph.value("connections").toArray()) {
        const auto edited=value.toObject(); QJsonObject edge;
        for(const auto &original:_document_template.value("connections").toArray())
            if(QtNodes::fromJson(original.toObject())==QtNodes::fromJson(edited)) { edge=original.toObject(); break; }
        for(auto it=edited.begin();it!=edited.end();++it) edge[it.key()]=it.value();
        connections.append(edge);
    }
    for(const auto &value:_unrendered_connections) connections.append(value);
    saved["connections"]=connections;
    if(!saved.contains("version")) saved["version"]=2;
    saved.remove("layout_pending");
    return pipeline::canonical_graph_document(saved);
}
void SchemeEditorWidget::retain_unrendered_connections() {
    _unrendered_connections={};
    auto rendered=_application.state().data_flow_graph->save().value("connections").toArray();
    for(const auto &value:_document_template.value("connections").toArray()) {
        const auto id=QtNodes::fromJson(value.toObject()); bool found=false;
        for(int i=0;i<rendered.size();++i) if(QtNodes::fromJson(rendered[i].toObject())==id) {
            rendered.removeAt(i); found=true; break;
        }
        if(!found) _unrendered_connections.append(value);
    }
    _unsupported_document |= !_unrendered_connections.isEmpty();
}
void SchemeEditorWidget::inspect_node(QtNodes::NodeId id) {
    if (!_inspector_layout) return;
    _inspected_node=id;
    while (auto *item=_inspector_layout->takeAt(0)) { delete item->widget(); delete item; }
    auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(model) set_inspector_expanded(true);
    if(model) {
        auto *title=new QLabel(model->caption(),this); title->setWordWrap(true);
        title->setObjectName("pipelineInspectorTitle");
        _inspector_layout->addWidget(title);
        _inspector_layout->addWidget(model->parameter_widget(this));
        _inspector_layout->addWidget(model->preview_widget(this));
    } else {
        auto *placeholder=ui::make<QLabel>("Select a node to see parameters",_inspector->widget());
        placeholder->setObjectName("pipelineInspectorPlaceholder"); placeholder->setWordWrap(true);
        _inspector_layout->addWidget(placeholder);
    }
    _inspector_layout->addStretch();
}
void SchemeEditorWidget::set_inspector_expanded(bool expanded) {
    if(!_splitter || _splitter->count()<3) return;
    auto sizes=_splitter->sizes();
    if(expanded && sizes[2]>0) return;
    if(!expanded) { _inspector_width=std::max(180,sizes[2]); sizes[1]+=sizes[2]; sizes[2]=0; }
    else { const int width=std::min(_inspector_width,std::max(180,sizes[1]/2)); sizes[1]=std::max(1,sizes[1]-width); sizes[2]=width; }
    _splitter->setSizes(sizes);
}
void SchemeEditorWidget::update_graph_view_indicator(QtNodes::NodeId id) {
    auto *item=_scene->nodeGraphicsObject(id);
    auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(!item || !model) return;
    GraphViewIndicator *indicator=nullptr;
    for(auto *child:item->childItems()) if(child->type()==GraphViewIndicator::Type) {
        indicator=static_cast<GraphViewIndicator *>(child); break;
    }
    if(pipeline::node_definition(model->name()).category!="views") { delete indicator; return; }
    if(!indicator) indicator=new GraphViewIndicator(item);
    const auto bounds=item->boundingRect();
    indicator->setPos(bounds.right()-24,bounds.top()+7);
    indicator->setToolTip(ui::text("Double-click to open graph"));
    indicator->update();
}
bool SchemeEditorWidget::save_preset(const QString &name) {
    const auto trimmed=name.trimmed(); if (trimmed.isEmpty()) return false;
    flush_presentation();
    auto graph=document();
    auto &presets=_application.state().file_configs.ui.pipeline_presets;
    presets.append(QJsonObject{{"version",1},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name",trimmed},{"graph",graph},{"view",_application.state().file_configs.ui.editor_view}});
    if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    return true;
}
bool SchemeEditorWidget::remove_preset(const QString &id) {
    auto &presets=_application.state().file_configs.ui.pipeline_presets;
    for (int i=0;i<presets.size();++i) if (presets[i].toObject().value("id").toString()==id) {
        presets.removeAt(i);
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
        return true;
    }
    return false;
}
void SchemeEditorWidget::load_document(const QJsonObject &document, const QJsonObject &view, bool active, bool switch_device) {
    if (!switch_device) flush_presentation();
    ++_document_generation;
    cancel_start_intent();
    _document_template = pipeline::canonical_graph_document(document);
    inspect_node(QtNodes::InvalidNodeId);
    _apply_request = 0;
    _save_timer.stop(); _apply_timer.stop(); _loading=true;
    auto &graph=*_application.state().data_flow_graph;
    {
        const QSignalBlocker graph_blocker(&graph),scene_blocker(_scene);
        const auto ids=graph.allNodeIds(); for (auto id : ids) graph.deleteNode(id);
        auto visible=editable_document(_document_template,_unsupported_document);
        visible["connections"]=QJsonArray{}; graph.load(visible);
        for(const auto &value:_document_template.value("connections").toArray()) {
            const auto connection=QtNodes::fromJson(value.toObject());
            if(graph.connectionPossible(connection)) graph.addConnection(connection);
        }
        retain_unrendered_connections();
        _scene->onModelReset(); _scene->undoStack().clear();
    }
    // This transaction deliberately blocks nodeCreated while rebuilding the
    // scene. Bind the replacement delegates explicitly once it is complete.
    for(auto id:graph.allNodeIds()) wire_source_model(id);
    for(auto id:graph.allNodeIds()) update_graph_view_indicator(id);
    // Only fresh standard documents request initial layout. Saved custom/active
    // documents have no marker, and their coordinates stay exactly as supplied.
    if (_document_template.value("layout_pending").toBool() && !_unsupported_document) {
        layout_nodes(false);
        _document_template.remove("layout_pending");
        if (view.isEmpty()) fit_nodes();
    }
    if (!view.isEmpty()) {
        const double scale=view.value("scale").toDouble(1),x=view.value("center_x").toDouble(),y=view.value("center_y").toDouble();
        if (std::isfinite(scale) && std::isfinite(x) && std::isfinite(y))
            static_cast<ProfiledGraphicsView *>(_view)->camera(std::clamp(scale,0.2,3.0),{x,y});
    }
    _view->setInteractive(!_unsupported_document);
    sync_preferences();
    if(auto *library=findChild<QTreeWidget *>("pipelineNodeLibrary")) library->setEnabled(!_unsupported_document);
    save_view();
    _loading=false;
    if (active || switch_device) {
        _application.state().editor_draft_modified = false;
        _last_processing_document = processing_document(graph.save());
    } else changed();
    if(_unsupported_document) set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
}
bool SchemeEditorWidget::load_preset(const QString &id) {
    if (_application.graph_request_pending()) return false;
    for (const auto &value : _application.state().file_configs.ui.pipeline_presets) {
        const auto preset=value.toObject();
        if (preset.value("id").toString()!=id || preset.value("version").toInt()!=1) continue;
        const auto document=preset.value("graph").toObject();
        if (!document.value("nodes").isArray() || !document.value("connections").isArray()) return false;
        QSet<int> ids;
        for (const auto &value : document.value("nodes").toArray()) {
            const auto node=value.toObject(); const int node_id=node.value("id").toInt(-1);
            if (node_id<0 || ids.contains(node_id)) return false;
            ids.insert(node_id);
        }
        load_document(document,preset.value("view").toObject());
        return true;
    }
    return false;
}
void SchemeEditorWidget::apply() {
    _apply_timer.stop();
    _application.state().editor_draft_modified = true;
    auto document = this->document();
    if (_application.state().file_configs.ui.autosave_enabled)
        emit _application.on_config_updated();
    QString error;
    if(_unsupported_document || !pipeline::GraphProgram::compile(document,error)) {
        _apply_request=0;
        set_status(_unsupported_document ? ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled.")
                                         : ui::text("Not applied: ")+error);
        return;
    }
    set_status({});
    _apply_request = (_default_start_intent || !_source_start_intents.isEmpty()) ? _application.apply_graph_and_start(document)
                                          : _application.apply_pipeline_graph(document);
}
void SchemeEditorWidget::cancel_start_intent() {
    _default_start_intent=false;
    _source_start_intents.clear();
}
void SchemeEditorWidget::wire_source_model(QtNodes::NodeId id) {
    if(auto *node=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))
        connect(node,&AudioNodeModel::local_source_output_changed,
                this,&SchemeEditorWidget::source_output_changed,Qt::UniqueConnection);
}
void SchemeEditorWidget::source_output_changed(bool enabled) {
    if(_loading) return;
    auto &graph=*_application.state().data_flow_graph;
    for(auto id:graph.allNodeIds()) if(graph.delegateModel<AudioNodeModel>(id)==sender()) {
        if(enabled) { _source_start_intents.insert(id); emit source_start_requested(); }
        else _source_start_intents.remove(id);
        return;
    }
}

void SchemeEditorWidget::load_active_pipeline() {
    _save_timer.stop();
    _apply_timer.stop();
    if (_application.graph_request_pending()) {
        set_status(ui::text("Wait for the pending graph command to finish before loading the active Data Flow."));
        return;
    }
    const auto document = _application.client().snapshot().graph;
    QString error;
    if (!pipeline::GraphProgram::compile(document, error)) {
        set_status(ui::text("Active Data Flow unavailable: ") + error);
        return;
    }
    load_document(document,{},true);
    set_status({});
}

SchemeEditorWidget::~SchemeEditorWidget() {
    flush_presentation();
    _loading = true; _save_timer.stop(); _apply_timer.stop();
    disconnect(_application.state().data_flow_graph.get(),nullptr,this,nullptr);
    disconnect(_scene,nullptr,this,nullptr);
    disconnect(&_scene->undoStack(),nullptr,this,nullptr);
    disconnect(&_application,nullptr,this,nullptr);
    disconnect(_view,nullptr,this,nullptr);
    for (auto *scroll : {_view->horizontalScrollBar(),_view->verticalScrollBar()}) disconnect(scroll,nullptr,this,nullptr);
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = {};
    static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions = {};
}
void SchemeEditorWidget::changed() {
    if (_loading) return;
    _application.state().editor_draft_modified = true;
    _save_timer.start();
    const auto processing = processing_document(_application.state().data_flow_graph->save());
    if (processing != _last_processing_document) {
        _last_processing_document = processing;
        _apply_timer.stop();
        _apply_request=0; // An acknowledgement of an older processing draft cannot label this one active.
        for (const auto id : _application.state().data_flow_graph->allNodeIds())
            if (auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) model->preview({});
        QString error;
        const bool valid=pipeline::GraphProgram::compile(document(),error)!=nullptr;
        set_status(valid ? QString{} : ui::text("Not applied: ")+error);
        if(_unsupported_document)
            set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
        else if(valid && _application.state().file_configs.ui.pipeline_auto_apply) _apply_timer.start();
    }
}
void SchemeEditorWidget::save_view() {
    static_cast<ProfiledGraphicsView *>(_view)->remember_camera();
    const auto center = static_cast<ProfiledGraphicsView *>(_view)->camera_center();
    auto &saved = _application.state().file_configs.ui.editor_view;
    saved["version"]=1; saved["scale"]=_view->getScale();
    saved["center_x"]=center.x(); saved["center_y"]=center.y();
}
void SchemeEditorWidget::flush_presentation() {
    auto &sync = static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions;
    if (sync) sync();
    _save_timer.stop(); save_view();
    if (_application.state().file_configs.ui.autosave_enabled && _application.state().editor_draft_modified)
        emit _application.on_config_updated();
}
void SchemeEditorWidget::arrange_nodes() {
    layout_nodes(true);
}
void SchemeEditorWidget::layout_nodes(bool undoable) {
    auto &graph = *_application.state().data_flow_graph;
    std::map<QtNodes::NodeId,int> incoming, layer;
    std::map<QtNodes::NodeId,std::vector<QtNodes::NodeId>> next;
    for (const auto id : graph.allNodeIds()) { incoming[id] = 0; layer[id] = 0; }
    for (const auto value : graph.save().value("connections").toArray()) {
        const auto edge = value.toObject();
        const auto from = QtNodes::NodeId(edge.value("outNodeId").toInt()), to = QtNodes::NodeId(edge.value("inNodeId").toInt());
        if (incoming.contains(from) && incoming.contains(to)) { ++incoming[to]; next[from].push_back(to); }
    }
    std::queue<QtNodes::NodeId> queue;
    for (const auto &[id,count] : incoming) if (!count) queue.push(id);
    while (!queue.empty()) {
        const auto id = queue.front(); queue.pop();
        for (auto to : next[id]) { layer[to] = std::max(layer[to],layer[id]+1); if (--incoming[to] == 0) queue.push(to); }
    }
    // Cyclic drafts are still arrangable; placement does not validate/apply DSP.
    std::map<int,std::vector<QtNodes::NodeId>> columns;
    for (const auto &[id,column] : layer) columns[column].push_back(id);
    std::map<QtNodes::NodeId,QPointF> positions;
    double x = 0;
    for (auto &[column,ids] : columns) {
        auto rank=[&](QtNodes::NodeId id) {
            const auto *model=graph.delegateModel<AudioNodeModel>(id);
            const auto category=model ? pipeline::node_definition(model->name()).category : QString();
            return category=="inputs" ? 0 : category=="transforms" ? 1 : category=="views" ? 2 : 3;
        };
        std::stable_sort(ids.begin(),ids.end(),[&](auto a,auto b) { return rank(a)<rank(b); });
        double y = 0, width = 0;
        for (const auto id : ids) if (auto *item = _scene->nodeGraphicsObject(id)) {
            const auto bounds = item->boundingRect();
            positions[id] = QPointF(x - bounds.left(), y - bounds.top());
            width = std::max(width,bounds.width()); y += bounds.height() + 40;
        }
        x += width + 40;
    }
    if (!positions.empty()) {
        if (undoable) _scene->undoStack().push(new ArrangeCommand(graph,std::move(positions)));
        else for (const auto &[id,position]:positions) graph.setNodeData(id,QtNodes::NodeRole::Position,position);
    }
}
void SchemeEditorWidget::fit_nodes() {
    const auto nodes=node_bounds(); if(nodes.isEmpty()) return;
    const auto bounds=nodes.adjusted(-40,-40,40,40);
    const double scale=std::clamp(std::min(_view->viewport()->width()/bounds.width(),_view->viewport()->height()/bounds.height()),0.2,3.0);
    static_cast<ProfiledGraphicsView *>(_view)->camera(scale,bounds.center());
    if (!_loading) _save_timer.start();
}
QRectF SchemeEditorWidget::node_bounds() const {
    QRectF bounds;
    for (const auto id : _application.state().data_flow_graph->allNodeIds())
        if (auto *item = _scene->nodeGraphicsObject(id)) bounds = bounds.united(item->sceneBoundingRect());
    return bounds;
}
