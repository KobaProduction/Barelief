#include "visualization/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/wheel_input.h"
#include "core_app/touchpad_input.h"
#include <QNativeGestureEvent>

#include <QtNodes/ConnectionStyle>
#include <QtNodes/GraphicsViewStyle>
#include <QtNodes/StyleCollection>
#include <QApplication>
#include <QSet>
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
#include "core_app/scheme_editor/numeric_editor_model.h"
#include "core_app/scheme_editor/editor_geometry.h"
#include "core_app/scheme_editor/editor_connection_painter.h"
#include <QToolTip>
#include <desktop/numeric_project_document.h>
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
#include <QSpinBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QTextEdit>
#include <QPlainTextEdit>
#include <QGraphicsProxyWidget>
#include <QToolButton>
#include <QMenu>
#include <QWidgetAction>
#include <QStyle>
#include <QClipboard>
#include <QMimeData>
#include <QInputDialog>
#include <QUuid>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionGraphicsObject.hpp>
#include <QtNodes/internal/ConnectionIdUtils.hpp>
#include <QtNodes/internal/UndoCommands.hpp>
#include <QTreeWidget>
#include <QScrollArea>
#include <QSplitter>
#include <limits>
#include <tuple>
static bool graph_available(const AudioNodeModel*);
#include <QSplitterHandle>
#include "pipeline/test_signal.h"
#include <map>
#include <queue>
#include <algorithm>
#include <array>
#include <cmath>

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

static bool trim_clipboard_connections() {
    const auto *mime=QApplication::clipboard()->mimeData();
    if(!mime) return false;
    const auto bytes=mime->hasFormat("application/qt-nodes-graph")
        ? mime->data("application/qt-nodes-graph") : mime->text().toUtf8();
      if(bytes.size()>16*1024*1024)return false;
    auto document=QJsonDocument::fromJson(bytes).object();
    const auto nodes=document.value("nodes").toArray();
    if(!document.value("nodes").isArray() || nodes.isEmpty() || nodes.size()>int(numeric_editor::limits().max_nodes) ||
       !document.value("connections").isArray() || document.value("connections").toArray().size()>int(numeric_editor::limits().max_edges))return false;
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto& kind:numeric_editor::catalog())registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);});
    NumericEditorGraphModel candidate(registry);
    for(const auto value:nodes) {
        const auto node=value.toObject();const auto id=node.value("id").toInteger(-1);
        const auto kind=node.value("internal-data").toObject().value("model-name").toString();
        if(!value.isObject() || id<0 || std::uint64_t(id)>=std::numeric_limits<QtNodes::NodeId>::max() ||
           node.value("id").toDouble()!=double(id) || candidate.nodeExists(QtNodes::NodeId(id)) || !numeric_editor::catalog().contains(kind))return false;
        const auto position=node.value("position").toObject();
        if(!position.value("x").isDouble() || !position.value("y").isDouble() || !std::isfinite(position.value("x").toDouble()) || !std::isfinite(position.value("y").toDouble()))return false;
        const auto presentation=node.value("internal-data").toObject().value("presentation");
        if((!presentation.isUndefined() && !presentation.isObject()) || !AudioNodeModel::valid_presentation(presentation.toObject()))return false;
        candidate.loadNode(node);
    }
    domain::NumericProcessingGraph checked;if(!candidate.export_numeric_graph(checked))return false;
      const auto groups=document.value("groups");
      if(!groups.isUndefined() && (!groups.isArray() || groups.toArray().size()>nodes.size()))return false;
      QSet<qint64> groupIds,groupedNodes;
      for(const auto value:groups.toArray()) {
          const auto group=value.toObject();const auto id=group.value("id").toInteger(-1);
          if(!value.isObject() || !group.value("id").isDouble() || id<0 || group.value("id").toDouble()!=double(id) || groupIds.contains(id) ||
             !group.value("name").isString() || group.value("name").toString().size()>1024 || !group.value("nodes").isArray())return false;
          groupIds.insert(id);
          for(const auto member:group.value("nodes").toArray()) {
              const auto node=member.toInteger(-1);
              if(!member.isDouble() || node<0 || std::uint64_t(node)>=std::numeric_limits<QtNodes::NodeId>::max() || member.toDouble()!=double(node) ||
                 !candidate.nodeExists(QtNodes::NodeId(node)) || groupedNodes.contains(node))return false;
              groupedNodes.insert(node);
          }
      }
    QJsonArray connections;
    for(const auto value:document.value("connections").toArray()) {
        if(!value.isObject())return false;
          for(const auto* key:{"outNodeId","inNodeId","outPortIndex","inPortIndex"}) {
              const auto field=value.toObject().value(key);const auto id=field.toInteger(-1);
              if(!field.isDouble() || id<0 || std::uint64_t(id)>=std::numeric_limits<QtNodes::NodeId>::max() || field.toDouble()!=double(id))return false;
          }
        const auto policy=value.toObject().value("conversion");
        if(!policy.isUndefined() && (!policy.isDouble() || policy.toInt(-1)<0 || policy.toInt(-1)>3 || policy.toDouble()!=policy.toInt(-1)))return false;
        const auto id=QtNodes::fromJson(value.toObject());
        if(!candidate.nodeExists(id.outNodeId) || !candidate.nodeExists(id.inNodeId))continue;
          if(candidate.connectionExists(id))continue; // Copy enumerates both selected endpoints.
        if(!candidate.connectionPossible(id))return false;
        candidate.addConnection(id);connections.append(value);
    }
    // QtNodes Copy includes incident edges to unselected nodes. Its Paste then
    // remaps absent endpoints to ID 0, producing invalid links/failed insertion.
    if(connections==document.value("connections").toArray()) return true;
    document["connections"]=connections;
    const auto corrected=QJsonDocument(document).toJson(QJsonDocument::Compact);
    auto *result=new QMimeData; result->setData("application/qt-nodes-graph",corrected);
    result->setText(QString::fromUtf8(corrected)); QApplication::clipboard()->setMimeData(result);
    return true;
}

class LocalizedDataFlowScene final : public DataFlowGraphicsScene {
public:
    using DataFlowGraphicsScene::DataFlowGraphicsScene;
    std::function<void(int)> focus_graph;
    std::function<void(int,int)> open_graph;
    std::function<void(int,QMenu *)> graph_menu;
    QMenu *createStdMenu(QPointF scene_position) {
        auto *menu=DataFlowGraphicsScene::createStdMenu(scene_position);
        for(auto* item:items(scene_position))if(auto* wire=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            auto* graph=dynamic_cast<NumericEditorGraphModel*>(&graphModel());if(!graph)break;
            const auto edge=wire->connectionId();
            auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);
            if(!from || !to)break;
            auto* detail=menu->addAction(from->caption()+" → "+to->caption()+"\n"+from->portCaption(QtNodes::PortType::Out,edge.outPortIndex)+" → "+to->portCaption(QtNodes::PortType::In,edge.inPortIndex));detail->setEnabled(false);
            auto* policies=menu->addMenu(ui::text("Connection conversion"));
            auto* status=menu->addAction(graph->connection_status(edge));status->setEnabled(false);
            for(std::uint8_t mode=0;mode<4;++mode){auto* action=policies->addAction(numeric_editor::conversion_policy_caption(mode));action->setCheckable(true);action->setChecked(graph->connection_policy(edge)==mode);connect(action,&QAction::triggered,this,[graph,edge,mode]{graph->edit_connection_policy(edge,mode);});}
            break;
        }
        for(auto *action:menu->actions()) {
            if(action->text()=="Copy") ui::bind(action,"text","Copy");
            else if(action->text()=="Cut") ui::bind(action,"text","Cut");
            connect(action,&QAction::triggered,this,[] { trim_clipboard_connections(); });
        }
        auto *paste=menu->addAction(ui::text("Paste")); paste->setShortcut(QKeySequence::Paste);
        connect(paste,&QAction::triggered,this,[this,scene_position] {
            if(!trim_clipboard_connections())return;
            undoStack().push(new QtNodes::PasteCommand(this,scene_position));
        });
        auto *remove=menu->addAction(ui::text("Delete selection"));
        remove->setObjectName("deleteGraphSelection"); remove->setShortcut(QKeySequence(Qt::Key_Delete));
        connect(remove,&QAction::triggered,this,[this] { undoStack().push(new QtNodes::DeleteCommand(this)); });
        for(auto *item:items(scene_position)) {
            QtNodes::NodeGraphicsObject *node=nullptr;
            for(auto *parent=item;parent;parent=parent->parentItem())
                if((node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(parent))) break;
            if(!node) continue;
            auto &graph=static_cast<DataFlowGraphModel &>(graphModel());
            auto *model=graph.delegateModel<AudioNodeModel>(node->nodeId());
            if(model && model->name()=="InputSelector"){menu->addSeparator();model->populate_selector_menu(menu);break;}
            if(graph_available(model)) {
                menu->addSeparator(); const int id=int(node->nodeId());
                if(graph_menu) { graph_menu(id,menu); break; }
                auto *focus=menu->addAction(ui::text("Focus graph")); focus->setObjectName("focusGraph");
                connect(focus,&QAction::triggered,this,[this,id] { if(focus_graph) focus_graph(id); });
                const std::array<const char *,3> labels{"Open in separate window","Open in new tab","Open beside current graph"};
                for(int placement=0;placement<3;++placement) {
                    auto *action=menu->addAction(ui::text(labels[placement]));
                    action->setObjectName(QStringLiteral("openGraphPlacement%1").arg(placement));
                    if (placement < 2) {
                        action->setShortcut(QKeySequence(placement == 0 ? "Alt+Shift+G" : "Alt+G"));
                        action->setShortcutVisibleInContextMenu(true);
                    }
                    connect(action,&QAction::triggered,this,[this,id,placement] { if(open_graph) open_graph(id,placement); });
                }
            }
            break;
        }
        return menu;
    }
    QMenu *createSceneMenu(QPointF scene_position) override {
        if(!items(scene_position).empty()) return createStdMenu(scene_position);
        auto* menu=new QMenu;auto* content=new QWidget(menu);auto* layout=new QVBoxLayout(content);
        auto* search=new QLineEdit(content);ui::bind(search,"placeholderText","Filter");layout->addWidget(search);
        auto* tree=new QTreeWidget(content);tree->setObjectName("numericContextCatalog");tree->setHeaderHidden(true);layout->addWidget(tree);
        numeric_editor::populate_catalog(tree);
        connect(&ui::Language::instance(),&ui::Language::changed,menu,[tree]{numeric_editor::populate_catalog(tree);});
        auto* action=new QWidgetAction(menu);action->setDefaultWidget(content);menu->addAction(action);
        connect(search,&QLineEdit::textChanged,tree,[tree](const QString& query){numeric_editor::filter_catalog(tree,query);});
        connect(tree,&QTreeWidget::itemDoubleClicked,this,[this,menu,scene_position](QTreeWidgetItem* item){const auto kind=item->data(0,Qt::UserRole).toString();if(kind.isEmpty())return;undoStack().push(new QtNodes::CreateCommand(this,kind,scene_position));menu->close();});
        return menu;
    }
};

void SchemeEditorWidget::set_graph_menu_provider(std::function<void(int,QMenu *)> provider) {
    static_cast<LocalizedDataFlowScene *>(_scene)->graph_menu=std::move(provider);
}
void SchemeEditorWidget::set_preset_presentation_provider(std::function<QJsonObject()> provider) {
    _preset_presentation_provider=std::move(provider);
}

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
        desktop_input::track_wheel_devices();
        desktop_input::attach_touchpad(viewport(),[this](QPoint pos) { return !editing_widget_at(pos); });
        setMouseTracking(true); viewport()->setMouseTracking(true);
        for(auto *scroll:{horizontalScrollBar(),verticalScrollBar()})
            connect(scroll,&QScrollBar::valueChanged,this,[this] { if(!_restoring) remember_camera(); });
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
    std::function<void()> on_connection_rejected;
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
        if(!trim_clipboard_connections())return;
        nodeScene()->undoStack().push(new QtNodes::PasteCommand(nodeScene(),mapToScene(point)));
    }
    void set_navigation_speed(float speed) {
        _navigation_speed=std::isfinite(speed) ? std::clamp(speed,0.5f,10.0f) : 3.0f;
    }
    void configure_graph_shortcuts() {
        for(auto *action:actions()) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            action->setAutoRepeat(false);
            const auto shortcuts=action->shortcuts();
            if(shortcuts.contains(QKeySequence(QKeySequence::Undo))) {
                action->setShortcuts({}); // Shared File menu owns history shortcuts.
            } else if(shortcuts.contains(QKeySequence(QKeySequence::Redo))) {
                action->setShortcuts({});
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
                if(stable=="Delete Selection") action->setShortcuts({QKeySequence(Qt::Key_Delete),QKeySequence(Qt::Key_Backspace)});
            }
        }
    }
    void remember_camera() {
        if (!_restoring && isVisible()) _center=mapToScene(viewport()->rect().center());
    }
    QPointF camera_center() const { return _center; }
    void camera(double scale, QPointF center, bool restore = true) {
        _restoring = true;
        // setupScale skips equal scales; reset also removes old zoom translations.
        const bool scaleChangedNow=std::abs(scale-getScale())>1e-9;
        if(scaleChangedNow) { QTransform transform; transform.scale(scale,scale); setTransform(transform); }
        auto bounds=nodeScene()->itemsBoundingRect();
        if(bounds.isEmpty()) bounds=QRectF(-100,-100,200,200);
        const auto half=QSizeF(viewport()->size())/(2*scale);
        // Saved cameras and pointer-anchored zoom may look beyond the nodes.
        // Keep that viewport reachable; pan stays bounded around this anchor.
        // Do not move the anchor on each pan: that would permit unlimited drift.
        if(restore) _camera_anchor=center;
        bounds=bounds.united(QRectF(_camera_anchor-QPointF(half.width(),half.height()),half*2));
        const auto size=QSizeF(std::max(bounds.width(),2*half.width()),std::max(bounds.height(),2*half.height()));
        bounds=QRectF(bounds.center()-QPointF(size.width()/2,size.height()/2),size);
        const double margin=160/scale;
        bounds.adjust(-margin,-margin,margin,margin);
        setSceneRect(bounds);
        center.setX(std::clamp(center.x(),bounds.left()+half.width(),bounds.right()-half.width()));
        center.setY(std::clamp(center.y(),bounds.top()+half.height(),bounds.bottom()-half.height()));
        centerOn(center); _center = center;
        _restoring = false;
        if(scaleChangedNow) emit scaleChanged(getScale());
    }

  protected:
    void contextMenuEvent(QContextMenuEvent *event) override {
        QGraphicsView::contextMenuEvent(event);
    }
    bool viewportEvent(QEvent *event) override {
        if(event->type()==QEvent::NativeGesture) {
            auto *gesture=static_cast<QNativeGestureEvent *>(event);
            if(gesture->gestureType()==Qt::PanNativeGesture) {
                pan_pixels(-gesture->delta()); event->accept(); return true;
            }
            if(gesture->gestureType()==Qt::ZoomNativeGesture) {
                const double next=std::clamp(getScale()*(1+gesture->value()),0.2,3.0);
                const auto anchor=mapToScene(gesture->position().toPoint());
                const auto offset=(gesture->position()-QPointF(viewport()->rect().center()))/next;
                camera(next,anchor-offset);
                event->accept(); if(on_interaction) on_interaction(); return true;
            }
        }
        if(event->type()!=QEvent::ContextMenu) return GraphicsView::viewportEvent(event);
        auto *context=static_cast<QContextMenuEvent *>(event);
        if(editing_widget_at(context->pos())) return GraphicsView::viewportEvent(event);
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
        QToolTip::hideText();_hover_wire=nullptr;
        _reorder_node=QtNodes::InvalidNodeId;
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
        QToolTip::hideText();_hover_wire=nullptr;
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
        if(event->button()==Qt::LeftButton && event->modifiers()==Qt::NoModifier && _pressed_node!=QtNodes::InvalidNodeId){
            auto* node=nodeScene()->nodeGraphicsObject(_pressed_node);const auto point=node->mapFromScene(mapToScene(event->position().toPoint()));
            auto& geometry=static_cast<EditorNodeGeometry&>(nodeScene()->nodeGeometry());const auto port=geometry.selectorButtonHit(_pressed_node,point);
            if(port!=QtNodes::InvalidPortIndex){auto* model=geometry.model(_pressed_node);const auto channels=model->numeric_node()->outputs.size();
                const auto input=model->port_id(QtNodes::PortType::In,port);model->select_input(channels>1?domain::PortId(input/channels):input);event->accept();return;}
        }
        stop_keyboard_pan();
        if(event->button()==Qt::LeftButton && event->modifiers().testFlag(Qt::ControlModifier) && _pressed_node!=QtNodes::InvalidNodeId){
            auto* node=nodeScene()->nodeGraphicsObject(_pressed_node);const auto point=node->mapFromScene(mapToScene(event->position().toPoint()));
            for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out}){const auto port=nodeScene()->nodeGeometry().checkPortHit(_pressed_node,side,point);
                if(port!=QtNodes::InvalidPortIndex){_reorder_node=_pressed_node;_reorder_side=side;_reorder_port=port;event->accept();return;}}
        }
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
        QtNodes::ConnectionGraphicsObject* hovered=nullptr;
        if(event->buttons()==Qt::NoButton)for(auto* item:items(event->position().toPoint()))if((hovered=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)))break;
        if(hovered!=_hover_wire){_hover_wire=hovered;if(hovered){auto* model=dynamic_cast<NumericEditorGraphModel*>(&hovered->graphModel());const auto text=ui::text("Click to inspect conversion")+(model && model->connection_visual_state(hovered->connectionId())>=2?"\n"+model->connection_status(hovered->connectionId()):QString{});QToolTip::showText(event->globalPosition().toPoint(),text,viewport());}else QToolTip::hideText();}
        if(_reorder_node!=QtNodes::InvalidNodeId){event->accept();return;}
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
    void leaveEvent(QEvent* event) override {QToolTip::hideText();_hover_wire=nullptr;GraphicsView::leaveEvent(event);}
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
        camera(getScale(),_center);
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
        camera(getScale(),_center);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        if(_reorder_node!=QtNodes::InvalidNodeId){
            auto* graph=dynamic_cast<NumericEditorGraphModel*>(&nodeScene()->graphModel());
            auto* node=nodeScene()->nodeGraphicsObject(_reorder_node);auto* model=graph?graph->delegateModel<AudioNodeModel>(_reorder_node):nullptr;
            if(node && model){auto order=model->visual_port_order(_reorder_side);const auto point=node->mapFromScene(mapToScene(_last_pointer));
                auto closest=_reorder_port;double distance=std::numeric_limits<double>::max();
                for(auto port:order){const auto delta=std::abs(nodeScene()->nodeGeometry().portPosition(_reorder_node,_reorder_side,port).y()-point.y());if(delta<distance){distance=delta;closest=port;}}
                const auto target=std::find(order.begin(),order.end(),closest)-order.begin();order.erase(std::find(order.begin(),order.end(),_reorder_port));order.insert(order.begin()+std::min<std::size_t>(target,order.size()),_reorder_port);
                QJsonArray ids;for(auto port:order)ids.append(int(model->port_id(_reorder_side,port)));auto presentation=model->presentation();presentation[_reorder_side==QtNodes::PortType::In?"input_port_order":"output_port_order"]=ids;
                graph->edit_presentation(_reorder_node,presentation);node->setGeometryChanged();node->updateQWidgetEmbedPos();node->moveConnections();node->update();
            }
            _reorder_node=QtNodes::InvalidNodeId;_pressed_node=QtNodes::InvalidNodeId;event->accept();if(on_interaction)on_interaction();return;
        }
        if(_middle_panning) {
            if(event->button()==Qt::MiddleButton) finish_middle_pan();
            event->accept(); return;
        }
        const bool wiring=scene()->mouseGrabberItem() && scene()->mouseGrabberItem()->type()==QtNodes::ConnectionGraphicsObject::Type;
        const auto undoIndex=static_cast<DataFlowGraphicsScene*>(scene())->undoStack().index();
        GraphicsView::mouseReleaseEvent(event);
        if(wiring && static_cast<DataFlowGraphicsScene*>(scene())->undoStack().index()==undoIndex && on_connection_rejected)on_connection_rejected();
        if(event->button()==Qt::LeftButton && !_press_alt && _pressed_node!=QtNodes::InvalidNodeId && !editing_control_focused() &&
           (_last_pointer-_press_pointer).manhattanLength()<QApplication::startDragDistance() && on_graph_clicked)
            on_graph_clicked(_pressed_node,_press_alt);
        _pressed_node=QtNodes::InvalidNodeId;
        remember_camera();
        if (synchronize_positions) synchronize_positions();
        if (on_interaction) on_interaction();
    }
    void wheelEvent(QWheelEvent *event) override {
        if(event->phase()==Qt::ScrollMomentum) { event->accept(); return; }
        const auto scroll=event->pixelDelta().isNull() ? QPointF(event->angleDelta())/3.0 : QPointF(event->pixelDelta());
        const bool touchpad=desktop_input::is_touchpad(*event);
        const bool pinch=desktop_input::is_pinch(*event);
        if(!touchpad && event->modifiers().testFlag(Qt::ControlModifier)) {
            pan_pixels({0,-scroll.y()}); event->accept(); return;
        }
        if(!touchpad && event->modifiers().testFlag(Qt::AltModifier)) {
            pan_pixels({-(scroll.y()!=0 ? scroll.y() : scroll.x()),0}); event->accept(); return;
        }
        if(touchpad && !pinch) {
            pan_pixels(-scroll); event->accept(); return;
        }
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
    QPointF _camera_anchor;
    bool _restoring = false;
    bool _middle_panning = false;
    QPoint _middle_last;
    QCursor _previous_cursor;
    QPoint _last_pointer, _press_pointer;
    bool _pointer_seen=false, _press_alt=false;
    QtNodes::NodeId _pressed_node=QtNodes::InvalidNodeId;
    QPointer<QtNodes::ConnectionGraphicsObject> _hover_wire;
    QtNodes::NodeId _reorder_node=QtNodes::InvalidNodeId;
    QtNodes::PortType _reorder_side=QtNodes::PortType::None;
    QtNodes::PortIndex _reorder_port=QtNodes::InvalidPortIndex;
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
    bool editing_widget_at(QPoint pos) const {
        for(auto *item:items(pos)) if(auto *proxy=qgraphicsitem_cast<QGraphicsProxyWidget *>(item)) {
            auto *widget=proxy->widget();
            auto *child=widget ? widget->childAt(proxy->mapFromScene(mapToScene(pos)).toPoint()) : nullptr;
            if(is_editing_widget(child)) return true;
        }
        return false;
    }
    void pan_pixels(QPointF delta) {
        camera(getScale(),_center+delta/getScale(),false);
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
    connection["UseDataDefinedColors"]=false;
    json["ConnectionStyle"]=connection;
    ConnectionStyle::setConnectionStyle(QString::fromUtf8(QJsonDocument(json).toJson(QJsonDocument::Compact)));
}

static bool graph_available(const AudioNodeModel* model) {
    if(!model || !model->numeric_node())return false;
    domain::ResolvedProcessingNode resolved;
    if(!domain::resolve_processing_node(*model->numeric_node(),numeric_editor::limits(),resolved))return false;
    return numeric_editor::is_graph_view(model->name()) && !resolved.inputs.empty();
}
static bool owned_processing_document(const QJsonObject& document,domain::NumericProcessingGraph& out,QString* error=nullptr) {
    // Snapshot decoding is separate from the canonical-only project writer.
    const auto bytes=QJsonDocument(document).toJson(QJsonDocument::Compact);
    const auto result=desktop::decode_numeric_editor_graph(bytes,out);
    if(!result && error)*error=result.error;
    return bool(result);
}
static QJsonObject processing_document(const QJsonObject& document) {
    domain::NumericProcessingGraph graph;
    if(!owned_processing_document(document,graph))return {{"invalid",true}};
    std::sort(graph.nodes.begin(),graph.nodes.end(),[](const auto& a,const auto& b){return a.id<b.id;});
    std::sort(graph.edges.begin(),graph.edges.end(),[](const auto& a,const auto& b){return std::tie(a.from,a.output_port,a.to,a.input_port)<std::tie(b.from,b.output_port,b.to,b.input_port);});
    // Qt enumerates connections without ordering. Configuration wires belong to
    // the same processing identity as data wires, independently of enumeration.
    std::sort(graph.configuration_bindings.begin(),graph.configuration_bindings.end(),[](const auto& a,const auto& b){return std::tie(a.from,a.output,a.to,a.input)<std::tie(b.from,b.output,b.to,b.input);});
    QByteArray bytes;if(!desktop::encode_numeric_graph(graph,bytes))return {{"invalid",true}};
    return QJsonDocument::fromJson(bytes).object();
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
    unsupported=!document.isEmpty() && (version!=5 || document.value("processing_version").toInt()!=4);
    for(const auto &value:document.value("nodes").toArray()) {
        const auto node=value.toObject();
        if(!numeric_editor::catalog().contains(node.value("internal-data").toObject().value("model-name").toString())) {
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

SchemeEditorWidget::SchemeEditorWidget(GuiPresenter &application, QWidget *parent)
    : QWidget(parent), _application(application),
      _scene(new LocalizedDataFlowScene(*application.state().data_flow_graph, this)),
      _view(new ProfiledGraphicsView(this)) {
    setObjectName("pipelineEditor");
    _document_template = application.state().file_configs.scene_graph.isEmpty()?application.state().data_flow_graph->save():application.state().file_configs.scene_graph;
    editable_document(_document_template,_unsupported_document);
    retain_unrendered_connections();
    setStyle__();
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this] {
        setStyle__();
        _view->setBackgroundBrush(QApplication::palette().color(QPalette::Window));
        _view->resetCachedContent();
        _scene->invalidate();_view->viewport()->update();
    });
    _scene->setNodeGeometry(std::make_unique<EditorNodeGeometry>(*_application.state().data_flow_graph,[this]{return _application.state().file_configs.ui.editor_view;}));
    _scene->setNodePainter(std::make_unique<EditorNodePainter>());
    _scene->setConnectionPainter(std::make_unique<EditorConnectionPainter>());
    _scene->setGroupingEnabled(false);
    _view->setScene(_scene);
    static_cast<ProfiledGraphicsView *>(_view)->configure_graph_shortcuts();
    _view->setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _view->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    _view->setScaleRange(0.2,3.0);
    _view->setTransformationAnchor(QGraphicsView::NoAnchor);
    _view->setResizeAnchor(QGraphicsView::NoAnchor);
    _view->setSceneRect(-10000000,-10000000,20000000,20000000);
    _view->setViewportUpdateMode(QGraphicsView::MinimalViewportUpdate);
    _view->setInteractive(!_unsupported_document);
    auto *layout = new QVBoxLayout(this);
    _toolbar = new QWidget(this);
    _toolbar->setObjectName("dataFlowToolbar");
    auto *bar = new QHBoxLayout(_toolbar);
    bar->setContentsMargins(0,0,0,0);
    bar->setSpacing(6);
    _toolbar->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);
    auto *apply_button = ui::make<QPushButton>("Apply", this);
    _apply_button=apply_button;
    apply_button->setObjectName("applyPipeline");
    ui::compact_control(apply_button);
    auto *presets=ui::tool_button(this,"Presets"); presets->setObjectName("pipelinePresets");
    _presets_button=presets; presets->hide();
    auto *preset_menu=new QMenu(presets); preset_menu->setObjectName("pipelinePresetsMenu"); ui::button_menu(presets,preset_menu);
    ui::bind(preset_menu,"title","Presets");
    auto action=[preset_menu](const char *text,const char *id) {
        auto *item=preset_menu->addAction(ui::text(text)); item->setObjectName(id); ui::bind(item,"text",text); return item;
    };
    auto *reset=action("Default","defaultPipeline");
    auto *generatorLoopback=action("Generator and capture source","generatorLoopbackPreset");
    connect(generatorLoopback,&QAction::triggered,this,[this] {
        if (_application.graph_request_pending()) {
            set_status(ui::text("Wait for the pending graph command to finish before loading the active Data Flow."));
            return;
        }
        emit generator_loopback_graph_requested();
    });
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
    connect(dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get()),&NumericEditorGraphModel::edit_rejected,this,[this](const QString& error){set_status(ui::text("Not applied: ")+error);});
    connect(test_source,&QAction::triggered,this,[this] {
        if (_application.graph_request_pending()) {
            set_status(ui::text("Wait for the pending graph command to finish before loading the active Data Flow."));
            return;
        }
        emit generator_loopback_graph_requested();
    });
    auto *execution=bar;
    _processing_rate=new QLabel(this);_processing_rate->setObjectName("pipelineProcessingRate");
    _processing_rate->setTextFormat(Qt::PlainText);
    execution->addWidget(_processing_rate);
    connect(&_application,&GuiPresenter::processing_state_changed,this,&SchemeEditorWidget::refresh_processing_telemetry);
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_processing_telemetry);
    refresh_processing_telemetry();
    auto *zoom=new QDoubleSpinBox(this); zoom->setObjectName("pipelineZoom");
    zoom->setMaximumWidth(72); zoom->setFixedHeight(24);
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
    ui::compact_control(fit);
    ui::bind(fit,"toolTip","Fit the Data Flow in view (Alt+F)");
    execution->addWidget(fit);
    connect(fit,&QPushButton::clicked,this,[this] {
        fit_nodes();
    });
    layout->setContentsMargins(3,3,3,3);
    layout->addWidget(_toolbar);
    auto *splitter = new InspectorSplitter(Qt::Horizontal,this);
    _splitter=splitter; splitter->setObjectName("pipelineSplitter");
    auto *library = new QTreeWidget(splitter); library->setObjectName("pipelineNodeLibrary");
    library->setBackgroundRole(QPalette::Window);
    library->viewport()->setBackgroundRole(QPalette::Window);
    library->viewport()->setAutoFillBackground(true);
    library->setHeaderHidden(true); library->setMinimumWidth(140);
    numeric_editor::populate_catalog(library);
    connect(&ui::Language::instance(),&ui::Language::changed,this,[this,library] {
        numeric_editor::populate_catalog(library);
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
        if(graph_available(model))
            emit graph_view_requested(int(id));
    });
    auto request_graph=[this](QtNodes::NodeId id,bool) {
        auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
        if(!graph_available(model)) return;
        emit graph_view_requested(int(id));
    };
    static_cast<ProfiledGraphicsView *>(_view)->on_graph_clicked=request_graph;
    auto *localized_scene=static_cast<LocalizedDataFlowScene *>(_scene);
    localized_scene->focus_graph=[request_graph](int id) { request_graph(QtNodes::NodeId(id),false); };
    localized_scene->open_graph=[this](int id,int placement) { emit graph_open_requested(id,placement); };
    connect(_scene,&QGraphicsScene::selectionChanged,this,[this] {
        for(auto* item:_scene->selectedItems())if(auto* wire=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)){inspect_connection(wire->connectionId());return;}
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
    _capture_status=new QLabel(_toolbar);_capture_status->setObjectName("pipelineCaptureStatus");
    _capture_status->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
    _capture_status->setTextInteractionFlags(Qt::NoTextInteraction);
    _capture_status->setTextFormat(Qt::PlainText);
    bar->addWidget(_capture_status);
    connect(&application,&GuiPresenter::component_status_changed,this,[this](const QString& component,int state,const QString& error){
        if(component!="capture")return;
        _capture_state=state;_capture_error=error;refresh_capture_status();
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_capture_status);
    connect(&application,&GuiPresenter::active_graph_changed,this,[this]{refresh_capture_status();});
    const auto capture=application.client().snapshot().statuses.value("capture");
    _capture_state=int(capture.state);_capture_error=capture.error;refresh_capture_status();

    connect(this,&SchemeEditorWidget::status_message,apply_button,&QPushButton::setToolTip);
    _apply_timer.setSingleShot(true);
    _apply_timer.setInterval(350);
    connect(&_apply_timer, &QTimer::timeout, this, [this] {
        apply();
    });
    connect(apply_button, &QPushButton::clicked, this, &SchemeEditorWidget::apply);
    connect(&application,&GuiPresenter::on_config_updated,this,&SchemeEditorWidget::sync_preferences);
    _auto_apply=application.state().file_configs.ui.pipeline_auto_apply;
    sync_preferences();
    connect(load_active, &QAction::triggered, this, &SchemeEditorWidget::load_active_pipeline);
    _last_processing_document = processing_document(application.state().data_flow_graph->save());
    _last_adapter_metadata=graph_adapter_metadata();
    _save_timer.setSingleShot(true); _save_timer.setInterval(350);
    connect(&_save_timer, &QTimer::timeout, this, [this] {
        save_view();
        if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());numeric && numeric->has_numeric_nodes()){emit numeric_draft_changed();return;}
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    });
    auto &model = *application.state().data_flow_graph;
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model))numeric->set_undo_stack(&_scene->undoStack());
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
    connect(_scene, &DataFlowGraphicsScene::modified, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodePositionUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeDeleted, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionDeleted, this, &SchemeEditorWidget::changed);
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model))connect(numeric,&NumericEditorGraphModel::atomic_edit_finished,this,&SchemeEditorWidget::changed);
    auto wire_details=[this]{auto* graph=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());if(!graph)return;
        for(auto id:graph->allNodeIds())for(auto edge:graph->allConnectionIds(id))if(edge.outNodeId==id){auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);auto* wire=_scene->connectionGraphicsObject(edge);
            if(from && to && wire){wire->setToolTip({});wire->update();}}
    };
    connect(&model,&DataFlowGraphModel::connectionCreated,this,wire_details);connect(&model,&DataFlowGraphModel::nodeUpdated,this,wire_details);connect(&ui::Language::instance(),&ui::Language::changed,this,wire_details);wire_details();
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model))connect(numeric,&NumericEditorGraphModel::connection_diagnostics_changed,this,wire_details);
    auto* display=ui::tool_button(this,"Editor display");auto* displayMenu=new QMenu(display);display->setMenu(displayMenu);display->setPopupMode(QToolButton::InstantPopup);bar->addWidget(display);
    auto* displayForm=new QWidget(displayMenu);displayForm->setProperty("scrollSafeControls",true);auto* fields=new QFormLayout(displayForm);
    struct Setting{const char* key;const char* title;int value,minimum,maximum;};
    for(const auto setting:{Setting{"node_width","Block width",200,140,400},Setting{"port_spacing","Port spacing",76,56,160},Setting{"port_radius","Port radius",7,5,14},Setting{"port_font","Port font size",9,7,16}}){auto* control=new QSpinBox(displayForm);control->setRange(setting.minimum,setting.maximum);control->setValue(_application.state().file_configs.ui.editor_view.value(setting.key).toInt(setting.value));fields->addRow(ui::text(setting.title),control);
        connect(control,&QSpinBox::valueChanged,this,[this,key=QString(setting.key)](int value){_application.state().file_configs.ui.editor_view[key]=value;
            for(auto id:_application.state().data_flow_graph->allNodeIds())if(auto* node=_scene->nodeGraphicsObject(id)){node->setGeometryChanged();_scene->nodeGeometry().recomputeSize(id);node->updateQWidgetEmbedPos();node->moveConnections();node->update();}save_view();_application.state().editor_draft_modified=true;_save_timer.start();});
    }
    auto* displayAction=new QWidgetAction(displayMenu);displayAction->setDefaultWidget(displayForm);displayMenu->addAction(displayAction);
    connect(&_scene->undoStack(), &QUndoStack::indexChanged, this, &SchemeEditorWidget::changed);
    auto *arrange = ui::make<QPushButton>("Arrange", this);
    ui::compact_control(arrange);
    ui::bind(arrange,"toolTip","Arrange nodes from left to right (Alt+A)");
    arrange->setObjectName("arrangePipeline"); bar->insertWidget(bar->indexOf(fit),arrange);
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
    const auto selected_graph = [this]() -> int {
        for (auto *item : _scene->selectedItems())
            if (auto *node = qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(item)) {
                auto *model = _application.state().data_flow_graph->delegateModel<AudioNodeModel>(node->nodeId());
                if (graph_available(model))
                    return int(node->nodeId());
            }
        return -1;
    };
    auto *showGraph = new QAction(this);
    showGraph->setObjectName("showSelectedGraph");
    ui::bind(showGraph,"text","Open in new tab");
    showGraph->setShortcut(QKeySequence("Alt+G"));
    showGraph->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(showGraph);
    connect(showGraph,&QAction::triggered,this,[this,selected_graph] {
        if (const auto id = selected_graph(); id >= 0) emit graph_open_requested(id,1);
    });
    auto *floatGraph = new QAction(this);
    floatGraph->setObjectName("floatSelectedGraph");
    ui::bind(floatGraph,"text","Open in separate window");
    floatGraph->setShortcut(QKeySequence("Alt+Shift+G"));
    floatGraph->setShortcutContext(Qt::WidgetWithChildrenShortcut); addAction(floatGraph);
    connect(floatGraph,&QAction::triggered,this,[this,selected_graph] {
        if (const auto id = selected_graph(); id >= 0) emit graph_open_requested(id,0);
    });
    connect(_view, &GraphicsView::scaleChanged, this, [this] { if (!_loading) _save_timer.start(); });
    for (auto *scroll : {_view->horizontalScrollBar(), _view->verticalScrollBar()})
        connect(scroll, &QScrollBar::sliderMoved, this, [this] { if (!_loading) _save_timer.start(); });
    _view->viewport()->installEventFilter(this);
    static_cast<ProfiledGraphicsView *>(_view)->on_connection_rejected=[this]{set_status(ui::text("Connection rejected: incompatible or unsupported ports"));};
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = [this] { if (!_loading) {_initial_layout_pending=false;_save_timer.start();} };
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
        emit default_graph_requested();
    });
    _preview_clock.start();
    _preview_revision=_application.client().snapshot().graph_revision;
    _preview_epoch=_application.client().snapshot().epoch;
    connect(&_application,&GuiPresenter::processing_state_changed,this,[this] {
        const auto& snapshot=_application.client().snapshot();
        if(snapshot.graph_revision==_preview_revision && snapshot.epoch==_preview_epoch)return;
        _preview_revision=snapshot.graph_revision;_preview_epoch=snapshot.epoch;
        clear_previews();sync_preview_subscription();
    });
    _active_processing_document=processing_document(_application.client().snapshot().graph);
    record_active_source_adapters(_application.client().snapshot().graph);
    connect(&_application,&GuiPresenter::active_graph_changed,this,[this](const QJsonObject& graph) {
        const auto processing=processing_document(graph);const bool processing_changed=processing!=_active_processing_document;
        _active_processing_document=processing;record_active_source_adapters(graph);
        if(_live_apply_pending) {
            bool adapters_match=true;
            for(const auto value:graph.value("nodes").toArray()){
                const auto saved=value.toObject();auto* node=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(QtNodes::NodeId(saved.value("id").toInt()));
                if(!node || node->adapter()!=saved.value("adapter").toObject()){adapters_match=false;break;}
            }
            if(processing_document(document())==processing && adapters_match) {
                _live_apply_pending=false;_apply_timer.stop();
            } else {
                // Edits may arrive during an Apply, including startup. Reconcile
                // the latest valid processing and adapter draft after its snapshot.
                _apply_timer.start();
            }
        }
        if(processing_changed)clear_previews();sync_preview_subscription();
        bool sources_active=true;for(auto id:_application.state().data_flow_graph->allNodeIds())sources_active&=preview_source_active(id);
        if(processing_document(document())==_active_processing_document && sources_active)set_status({});
    });
    connect(&_application,&GuiPresenter::numeric_frame_ready,this,[this](const pipeline::DeliveredNumericFrame& frame) {
        if(!accept_preview(frame,frame.status==signal_contracts::Status::ok))return;
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node))model->preview(frame);
    });
    connect(&_application,&GuiPresenter::numeric_history_ready,this,[this](const pipeline::DeliveredNumericHistoryRow& row) {
        if(!accept_preview(row.compact,row.event==pipeline::NumericHistoryEvent::Row))return;
        _application.client().request_numeric_snapshots();
    });
    connect(&_application,&GuiPresenter::numeric_history_snapshot_ready,this,[this](const pipeline::DeliveredNumericFrame& frame,quint64 ordinal,quint64 filled_count) {
        if(!accept_preview(frame,false))return;
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node))model->preview_history_snapshot(frame,ordinal,filled_count);
    });
    _preview_engine_state=int(_application.client().snapshot().statuses.value("analyzer").state);
    connect(&application,&GuiPresenter::component_status_changed,this,[this](const QString& component,int state,const QString&){
        if(component!="analyzer" || state==_preview_engine_state)return;
        _preview_engine_state=state;sync_preview_subscription();
    });
    connect(&application,&GuiPresenter::apply_status_changed,this,[this](const QString &status) {
        if(status.startsWith("Error: ")) set_status(ui::text("Not applied: ")+status.mid(7));
        else if(status.startsWith(ui::text("Not applied: "))) set_status(status);
    });
    connect(&application,&GuiPresenter::selected_device_changed,this,[this](const QJsonObject &draft,const QJsonObject &view) {
        _capture_state=int(pipeline::State::Stopped);_capture_error.clear();refresh_capture_status();
        load_document(draft,view,false,true);
    });
    connect(&application,&GuiPresenter::views_cleared,this,[this] {
        for (const auto id : _application.state().data_flow_graph->allNodeIds())
            if (auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) model->clear_preview();
        _preview_times.clear();
    });
    connect(&application, &GuiPresenter::graph_apply_finished, this,
            [this](quint64 request, bool accepted, const QString &error) {
                if (accepted) refresh_capture_status();
                if (request != _apply_request)
                    return;
                _apply_request = 0;
                if(accepted && processing_document(document())!=processing_document(_application.client().snapshot().graph))_apply_timer.start();
                if(!accepted){_live_apply_pending=false;_apply_timer.stop();}
                set_status(accepted ? QString{} : ui::text("Not applied: ") + error);
            });
    connect(&application, &GuiPresenter::saved_graph_warning_changed, this,
            [this](const QString &warning) { set_status(warning); });
    QString error;
    domain::NumericProcessingGraph draft;
    set_status(numeric_draft(draft,&error)?QString{}:ui::text("Draft not active: ")+error);
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
    QString diagnostic=text;
    const auto protection=_application.file_protection_warning();
    if(!protection.isEmpty() && !diagnostic.contains(protection))
        diagnostic=diagnostic.isEmpty()?protection:diagnostic+QStringLiteral("\n")+protection;
    _status->setToolTip(diagnostic);
    _status->setAccessibleName(diagnostic);
    _status->setAccessibleDescription(diagnostic);
    _status->setVisible(!diagnostic.isEmpty());
    emit status_message(diagnostic);
}
void SchemeEditorWidget::sync_preferences() {
    _auto_apply=true;
    _apply_button->setVisible(false);
}
QWidget* SchemeEditorWidget::execution_settings_widget(QWidget* parent) {
    auto* settings=new QWidget(parent);settings->setObjectName("pipelineExecutionSettings");
    auto* rate=new QLineEdit(settings);rate->setObjectName("pipelineAnalysisRate");rate->setMaximumWidth(96);
    ui::bind(rate,"toolTip","Analysis rate, Hz (integer or numerator/denominator)");
    auto* layout=new QVBoxLayout(settings);layout->setContentsMargins(0,0,0,0);
    layout->addWidget(ui::make<QLabel>("Analysis rate, Hz",settings));layout->addWidget(rate);
    auto* numericRate=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    auto refreshRate=[numericRate,rate]{if(!numericRate)return;const auto r=numericRate->numeric_graph().execution_interval;
        const QSignalBlocker blocker(rate);rate->setText(r.numerator==1?QString::number(r.denominator):QString::number(r.denominator)+"/"+QString::number(r.numerator));};
    refreshRate();connect(numericRate,&NumericEditorGraphModel::atomic_edit_finished,rate,refreshRate);
    connect(this,&SchemeEditorWidget::execution_settings_changed,rate,refreshRate);
    connect(rate,&QLineEdit::editingFinished,this,[this,numericRate,rate,refreshRate]{
        const auto parts=rate->text().trimmed().split('/');bool ok=false,ok2=true;
        const auto numerator=parts.value(0).toULongLong(&ok);const auto denominator=parts.size()==2?parts[1].toULongLong(&ok2):1;
        if(!numericRate || parts.size()>2 || !ok || !ok2 || !numerator || !denominator || denominator>std::uint64_t(INT64_MAX) ||
            !numericRate->edit_execution_interval({std::int64_t(denominator),numerator}))set_status(ui::text("Invalid execution interval"));
        refreshRate();
    });
    return settings;
}
void SchemeEditorWidget::refresh_processing_telemetry() {
    const auto snapshot=_application.client().snapshot();
    const auto telemetry=snapshot.diagnostics;
    const double hz=telemetry.value("processing_hz").toDouble();
    const bool measured=telemetry.value("processing_measured").toBool() && std::isfinite(hz) && hz>=0;
    const auto text=ui::text("Processing: %1 Hz").arg(measured?QString::number(hz,'f',1):QStringLiteral("—"));
    if(_processing_rate && _processing_rate->text()!=text)_processing_rate->setText(text);
    const QMap<QString,const char*> bases{{"window_outputs","Window outputs"},{"mixer_outputs","Mixer outputs"},{"transform_outputs","Transform outputs"},{"source_outputs","Source outputs"}};
    const auto basis=bases.value(telemetry.value("processing_rate_basis").toString(),"Awaiting measurement");
    if(_processing_rate)_processing_rate->setToolTip(ui::text("Measured processing output rate; independent of the analysis target and display FPS.")+"\n"+
        ui::text("Node %1 · %2 · interval %3 s").arg(measured?telemetry.value("processing_rate_node").toVariant().toString():QStringLiteral("—"),ui::text(basis),QString::number(telemetry.value("measurement_seconds").toDouble(),'f',2)));
    if(!isVisible())return;
    QMap<int,QJsonObject> rows;
    for(const auto& value:telemetry.value("node_stats").toArray()){const auto row=value.toObject();rows.insert(row.value("node").toInt(-1),row);}
    const bool current=_last_processing_document==_active_processing_document && snapshot.statuses.value("analyzer").state==pipeline::State::Running;
    for(auto id:_application.state().data_flow_graph->allNodeIds())if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) {
        const auto row=rows.value(int(id));
        model->set_processing_activity(current && measured && !row.isEmpty(),current && row.value("active").toBool(),row.value("calls_hz").toDouble());
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
        if(!numeric_editor::catalog().contains(node.value("internal-data").toObject().value("model-name").toString())) {
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
    return saved;
}
void SchemeEditorWidget::clear_previews() {
    _preview_times.clear();
    for(auto id:_application.state().data_flow_graph->allNodeIds())
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))model->clear_preview();
}
void SchemeEditorWidget::record_active_source_adapters(const QJsonObject& graph) {
    _active_source_adapters={};
    for(const auto& value:graph.value("nodes").toArray()){
        const auto node=value.toObject();const auto adapter=node.contains("adapter")?node.value("adapter").toObject():node.value("internal-data").toObject().value("adapter").toObject();
        _active_source_adapters[QString::number(node.value("id").toInteger())]=adapter;
    }
}
bool SchemeEditorWidget::preview_source_active(QtNodes::NodeId id) const {
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(!model || model->name()!="AudioSource")return true;
    return _active_source_adapters.value(QString::number(id)).toObject().value("device_identity").toString()==model->adapter().value("device_identity").toString();
}
void SchemeEditorWidget::refresh_preview_activity() {
    const auto state=_application.client().snapshot().statuses.value("analyzer").state;
    QString activity;
    if(state==pipeline::State::Stopped)activity="Processing stopped";
    else if(_last_processing_document!=_active_processing_document)activity="Draft not active";
    else if(state==pipeline::State::Starting || state==pipeline::State::Recovering)activity="Processing starting";
    else if(state==pipeline::State::Failed)activity="Processing failed";
    for(auto id:_application.state().data_flow_graph->allNodeIds())if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))model->set_preview_activity(activity.isEmpty() && !preview_source_active(id)?QStringLiteral("Draft not active"):activity);
}
void SchemeEditorWidget::sync_preview_subscription() {
    refresh_preview_activity();
    std::vector<pipeline::EndpointIdentity> endpoints;
    domain::NumericProcessingGraph draft;
    if(isVisible() && _last_processing_document==_active_processing_document && numeric_draft(draft))
        for(const auto& node:draft.nodes)if(preview_source_active(QtNodes::NodeId(node.id))) {
            domain::ResolvedProcessingNode resolved;if(!domain::resolve_processing_node(node,numeric_editor::limits(),resolved))continue;
            const auto& ports=resolved.role==domain::NodeRole::Sink?node.inputs:node.outputs;
            for(const auto& port:ports)endpoints.push_back({draft.graph_id,node.id,port.direction,port.id});
        }
    _application.set_editor_numeric_endpoints(std::move(endpoints));
}
void SchemeEditorWidget::showEvent(QShowEvent* event) { QWidget::showEvent(event);sync_preview_subscription();refresh_processing_telemetry();schedule_initial_layout(); }
void SchemeEditorWidget::hideEvent(QHideEvent* event) { QWidget::hideEvent(event);clear_previews();sync_preview_subscription(); }
bool SchemeEditorWidget::eventFilter(QObject* object,QEvent* event) {
    if(_view && object==_view->viewport() && (event->type()==QEvent::Resize || event->type()==QEvent::Show))schedule_initial_layout();
    return QWidget::eventFilter(object,event);
}
void SchemeEditorWidget::initialize_default_layout() { _initial_layout_pending=true;_initial_layout_fit=true;schedule_initial_layout(); }
void SchemeEditorWidget::schedule_initial_layout() {
    if(!_initial_layout_pending || _initial_layout_scheduled)return;
    _initial_layout_scheduled=true;const auto generation=_document_generation;
    QTimer::singleShot(0,this,[this,generation]{
        _initial_layout_scheduled=false;
        if(generation!=_document_generation){schedule_initial_layout();return;}
        if(!_initial_layout_pending || !isVisible() || _view->viewport()->width()<2 || _view->viewport()->height()<2)return;
        _initial_layout_pending=false;_loading=true;layout_nodes(false);if(_initial_layout_fit)fit_nodes();_loading=false;
        save_view();_application.state().editor_draft_modified=true;_save_timer.start();
    });
}
bool SchemeEditorWidget::accept_preview(const pipeline::DeliveredNumericFrame& frame,bool throttle) {
    if(!isVisible() || !frame.context || _last_processing_document!=_active_processing_document)return false;
    const auto snapshot=_application.client().snapshot();
    if(snapshot.statuses.value("analyzer").state!=pipeline::State::Running)return false;
    if(frame.delivery.epoch!=snapshot.epoch || frame.delivery.graph_revision!=snapshot.graph_revision ||
       frame.context->endpoint.graph_id!=_active_processing_document.value("graph_id").toString().toStdString())return false;
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node);
    if(!model || !model->numeric_node() || !preview_source_active(QtNodes::NodeId(frame.context->endpoint.node)))return false;
    const auto& node=*model->numeric_node();const auto& endpoint=frame.context->endpoint;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?node.outputs:node.inputs;
    if(std::none_of(ports.begin(),ports.end(),[&](const auto& port){return port.id==endpoint.port;}))return false;
    const auto now=_preview_clock.elapsed();const auto found=_preview_times.find(endpoint);
    if(throttle && found!=_preview_times.end() && now-found->second<100)return false;
    _preview_times[endpoint]=now;return true;
}
bool SchemeEditorWidget::numeric_draft(domain::NumericProcessingGraph& out,QString* error) const {
    auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    if(!numeric){if(error)*error=ui::text("Numeric editor unavailable");return false;}
    if(_unsupported_document){if(error)*error=ui::text("Draft contains unsupported nodes");return false;}
    return numeric->export_numeric_graph(out,error);
}
QJsonObject SchemeEditorWidget::graph_adapter_metadata() const {
    auto& graph=*_application.state().data_flow_graph;auto ids=graph.allNodeIds();std::vector<QtNodes::NodeId> sorted(ids.begin(),ids.end());std::sort(sorted.begin(),sorted.end());QJsonArray nodes;
    for(auto id:sorted)if(auto* model=graph.delegateModel<AudioNodeModel>(id))nodes.append(QJsonObject{{"id",qint64(id)},{"adapter",model->adapter()}});
    return {{"graph_id",graph.save().value("graph_id")},{"nodes",nodes}};
}
bool SchemeEditorWidget::restore_graph_adapters(const QJsonObject& node_adapters,QString* error) {
    auto& graph=*_application.state().data_flow_graph;QMap<QtNodes::NodeId,QJsonObject> checked;
    for(auto it=node_adapters.begin();it!=node_adapters.end();++it){bool valid=false;const auto id=it.key().toUInt(&valid);QJsonObject adapter;
        if(!valid || QString::number(id)!=it.key() || !graph.delegateModel<AudioNodeModel>(id) || !it.value().isObject()){if(error)*error=ui::text("Invalid network source settings");return false;}
        const auto result=desktop::decode_numeric_node_adapter(QJsonDocument(it.value().toObject()).toJson(QJsonDocument::Compact),adapter);if(!result){if(error)*error=result.error;return false;}
        checked[id]=std::move(adapter);
    }
    QMap<QtNodes::NodeId,QJsonObject> previous;
    for(auto it=checked.begin();it!=checked.end();++it)previous[it.key()]=graph.delegateModel<AudioNodeModel>(it.key())->adapter();
    const auto prior=_loading;_loading=true;
    for(auto it=checked.begin();it!=checked.end();++it)graph.delegateModel<AudioNodeModel>(it.key())->set_adapter(it.value());
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&graph);numeric && !numeric->synchronize_native_sources(error)) {
        for(auto it=previous.begin();it!=previous.end();++it)graph.delegateModel<AudioNodeModel>(it.key())->set_adapter(it.value());
        _loading=prior;return false;
    }
    _last_processing_document=processing_document(graph.save());
    _loading=prior;_last_adapter_metadata=graph_adapter_metadata();sync_preview_subscription();return true;
}
bool SchemeEditorWidget::load_numeric_draft(const domain::NumericProcessingGraph& graph,QString* error,const QJsonObject& node_adapters) {
    auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());if(!numeric){if(error)*error=ui::text("Numeric editor unavailable");return false;}
    domain::NumericProcessingGraph prepared;
    if(!numeric->prepare_numeric_import(graph,node_adapters,prepared,error))return false;
    NumericEditorGraphModel candidate(numeric->dataModelRegistry());
    if(!candidate.set_numeric_graph(prepared,error,node_adapters))return false;
    // Complete the model transaction before altering inspector, pending work or
    // Undo. A native discovery failure must leave the entire editor untouched.
    const auto prior_loading=_loading;_loading=true;
    {QSignalBlocker graph_blocker(numeric),scene_blocker(_scene);
        if(!numeric->set_numeric_graph(prepared,error,node_adapters)){_loading=prior_loading;return false;}
        ++_document_generation;_initial_layout_pending=false;_live_apply_pending=false;
        _apply_timer.stop();_save_timer.stop();_preview_times.clear();inspect_node(QtNodes::InvalidNodeId);
        _scene->onModelReset();_scene->undoStack().clear();}
    _document_template=numeric->save();_unrendered_connections={};_unsupported_document=false;
    _view->setInteractive(true);if(auto* library=findChild<QTreeWidget*>("pipelineNodeLibrary"))library->setEnabled(true);
    for(auto id:numeric->allNodeIds())wire_source_model(id);
    _last_processing_document=processing_document(numeric->save());
    _last_adapter_metadata=graph_adapter_metadata();
    _loading=false;_application.state().editor_draft_modified=false;set_status({});
    emit execution_settings_changed();
    sync_preview_subscription();
    return true;
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
    if(model) {
        auto *title=new QLabel(model->caption(),this); title->setWordWrap(true);
        title->setTextFormat(Qt::PlainText);
        title->setObjectName("pipelineInspectorTitle");
        title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        title->setMinimumWidth(0);
        _inspector_layout->addWidget(title);
        connect(model,&AudioNodeModel::presentation_changed,title,[model,title]{title->setText(model->caption());});
        auto* inputs=new QLabel(this);inputs->setObjectName("pipelineInputSummary");inputs->setTextFormat(Qt::PlainText);inputs->setWordWrap(true);
        auto refreshInputs=[model,inputs]{QStringList descriptions;
            if(const auto* node=model->numeric_node())for(const auto& port:node->inputs)
                descriptions.append(ui::text("Input port %1").arg(port.id)+" · "+numeric_editor::port_caption(port));
            inputs->setText(descriptions.join('\n'));inputs->setVisible(!descriptions.isEmpty());};
        refreshInputs();connect(&ui::Language::instance(),&ui::Language::changed,inputs,refreshInputs);
        connect(model,&AudioNodeModel::parameter_structure_changed,inputs,refreshInputs);
        _inspector_layout->addWidget(inputs);
        _inspector_layout->addWidget(model->preview_widget(this));
        _inspector_layout->addWidget(model->parameter_widget(this));
        if(numeric_editor::is_graph_view(model->name())) {
            auto* rename=ui::make<QPushButton>("Rename graph",this);
            rename->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            rename->setObjectName("pipelineRenameGraph");
            _inspector_layout->addWidget(rename);
            connect(rename,&QPushButton::clicked,this,[this,id]{emit graph_rename_requested(int(id));});
        }else{
            auto* rename=ui::make<QPushButton>("Rename block",this);rename->setObjectName("pipelineRenameNode");
            rename->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
            _inspector_layout->addWidget(rename);
            connect(rename,&QPushButton::clicked,this,[this,id]{
                auto* current=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);if(!current)return;
                bool accepted=false;const auto name=QInputDialog::getText(this,ui::text("Rename block"),ui::text("Block name (empty restores default)"),QLineEdit::Normal,current->presentation().value("display_name").toString(),&accepted);
                if(accepted)current->edit_display_name(name);
            });
        }
        auto* diagnostics=new QWidget(this);diagnostics->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);auto* diagnosticLayout=new QVBoxLayout(diagnostics);diagnosticLayout->setContentsMargins(0,0,0,0);
        auto* heading=ui::make<QLabel>("Diagnostics / display",diagnostics);heading->setWordWrap(true);diagnosticLayout->addWidget(heading);
        auto* timing=new QLabel(diagnostics);timing->setObjectName("pipelineNodeTiming");timing->setWordWrap(true);timing->setTextFormat(Qt::PlainText);diagnosticLayout->addWidget(timing);
        auto refreshTiming=[this,id,timing]{const auto telemetry=_application.client().snapshot().diagnostics;QJsonObject row;
            for(const auto& value:telemetry.value("node_stats").toArray())if(value.toObject().value("node").toInt(-1)==int(id)){row=value.toObject();break;}
            timing->setVisible(!row.isEmpty());if(row.isEmpty())return;
            const auto calls=row.value("calls").toString().toULongLong();const auto total=row.value("self_nanoseconds").toString().toULongLong();
            timing->setText(ui::text("Calls: %1 · errors: %2").arg(QString::number(calls),row.value("errors").toString())+"\n"+
                ui::text("Self time, average/max: %1 / %2 ms").arg(QString::number(calls?double(total)/double(calls)/1e6:0,'f',3),QString::number(row.value("max_self_nanoseconds").toString().toDouble()/1e6,'f',3))+"\n"+
                ui::text("Deadline misses: %1").arg(row.value("deadline_misses").toString()));};
        refreshTiming();connect(&_application,&GuiPresenter::processing_state_changed,timing,refreshTiming);
        _inspector_layout->addWidget(diagnostics);
    } else {
        auto *placeholder=ui::make<QLabel>("Select a node to see parameters",_inspector->widget());
        placeholder->setObjectName("pipelineInspectorPlaceholder"); placeholder->setWordWrap(true);
        _inspector_layout->addWidget(placeholder);
    }
    _inspector_layout->addStretch();
}
void SchemeEditorWidget::inspect_connection(QtNodes::ConnectionId edge) {
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    if(!graph || !graph->connectionExists(edge) || !_inspector_layout)return;
    _inspected_node=QtNodes::InvalidNodeId;
    while(auto* item=_inspector_layout->takeAt(0)){delete item->widget();delete item;}
    auto* panel=new QWidget(_inspector->widget());panel->setProperty("scrollSafeControls",true);auto* layout=new QVBoxLayout(panel);
    layout->setContentsMargins(0,0,0,0);
    auto* title=ui::make<QLabel>("Connection conversion",panel);title->setObjectName("pipelineInspectorTitle");layout->addWidget(title);
    auto* contracts=new QLabel(panel);contracts->setWordWrap(true);contracts->setTextFormat(Qt::PlainText);layout->addWidget(contracts);
    auto* risk=new QLabel(panel);risk->setWordWrap(true);risk->setTextFormat(Qt::PlainText);risk->setObjectName("connectionRisk");layout->addWidget(risk);
    auto* policy=new QComboBox(panel);policy->setObjectName("connectionConversionPolicy");layout->addWidget(policy);
    auto* effects=new QLabel(panel);effects->setWordWrap(true);effects->setTextFormat(Qt::PlainText);layout->addWidget(effects);
    auto* failure=new QLabel(panel);failure->setWordWrap(true);failure->setObjectName("connectionRuntimeError");layout->addWidget(failure);
    for(auto* label:{title,contracts,risk,effects,failure})label->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto refresh=[graph,edge,contracts,risk,policy,effects,failure]{
        if(!graph->connectionExists(edge))return;
        QSignalBlocker blocker(policy);policy->clear();for(std::uint8_t mode=0;mode<4;++mode)policy->addItem(numeric_editor::conversion_policy_caption(mode));policy->setCurrentIndex(graph->connection_policy(edge));
        auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);
        if(!from || !to || !from->numeric_node() || !to->numeric_node())return;
        const auto& outputs=from->numeric_node()->outputs;const auto& inputs=to->numeric_node()->inputs;
        const auto a=std::find_if(outputs.begin(),outputs.end(),[&](const auto& p){return p.id==edge.outPortIndex;});const auto b=std::find_if(inputs.begin(),inputs.end(),[&](const auto& p){return p.id==edge.inPortIndex;});
        if(a==outputs.end() || b==inputs.end())return;
        contracts->setText(from->caption()+" #"+QString::number(edge.outNodeId)+" → "+to->caption()+" #"+QString::number(edge.inNodeId)+"\n\n"+numeric_editor::contract_details(*a)+"\n↓\n"+numeric_editor::contract_details(*b));
        risk->setText(graph->connection_status(edge));effects->setText(numeric_editor::conversion_policy_details(graph->connection_policy(edge))+"\n\n"+graph->connection_effects(edge));
        const auto actual=graph->connection_failure(edge);failure->setText(actual.isEmpty()?ui::text("No observed runtime conversion error"):ui::text("Runtime error: %1").arg(actual));
        policy->setEnabled(!domain::processing_ports_compatible(*a,*b));
    };
    connect(policy,&QComboBox::currentIndexChanged,panel,[graph,edge](int mode){if(mode>=0)graph->edit_connection_policy(edge,std::uint8_t(mode));});
    connect(graph,&DataFlowGraphModel::nodeUpdated,panel,refresh);connect(&ui::Language::instance(),&ui::Language::changed,panel,refresh);
    for(auto id:{edge.outNodeId,edge.inNodeId})if(auto* node=graph->delegateModel<AudioNodeModel>(id))connect(node,&AudioNodeModel::diagnostics_changed,panel,[this,refresh]{refresh();_scene->invalidate();});
    connect(graph,&DataFlowGraphModel::connectionDeleted,panel,[this,edge](QtNodes::ConnectionId removed){if(removed==edge)inspect_node(QtNodes::InvalidNodeId);});
    refresh();_inspector_layout->addWidget(panel);_inspector_layout->addStretch();set_inspector_expanded(true);
}
void SchemeEditorWidget::set_inspector_expanded(bool expanded) {
    if(!_splitter || _splitter->count()<3) return;
    auto sizes=_splitter->sizes();
    if(expanded && sizes[2]>0) return;
    if(!expanded) {
        if(sizes[2]==0) return;
        _inspector_width=sizes[2]; sizes[1]+=sizes[2]; sizes[2]=0;
    } else {
        // Restore the user's width; QSplitter applies current child constraints.
        // Selection changes only inspector contents, never this layout state.
        const int available=std::max(1,sizes[1]-_view->minimumSizeHint().width());
        const int width=std::min(_inspector_width,available);
        sizes[1]-=width; sizes[2]=width;
    }
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
    if(!graph_available(model)) { delete indicator; return; }
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
    QJsonObject preset{{"version",1},{"id",QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {"name",trimmed},{"graph",graph},{"view",_application.state().file_configs.ui.editor_view}};
    if(_preset_presentation_provider){
        const auto presentation=_preset_presentation_provider();
        if(!presentation.isEmpty()){
            QJsonObject checked;
            if(!desktop::decode_numeric_preset_presentation(QJsonDocument(presentation).toJson(QJsonDocument::Compact),checked))return false;
            preset["presentation"]=checked;
        }
    }
    presets.append(preset);
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
bool SchemeEditorWidget::load_document(const QJsonObject &document, const QJsonObject &view, bool active, bool switch_device) {
    domain::NumericProcessingGraph incoming;QString error;
    if(!owned_processing_document(document,incoming,&error)){set_status(ui::text("Not applied: ")+error);return false;}
    auto visual=document;
    const auto nodes=document.value("nodes").toArray();QJsonObject nodeAdapters;
    for(const auto value:nodes){const auto node=value.toObject();const auto adapter=node.contains("kind")?node.value("adapter"):node.value("internal-data").toObject().value("adapter");QJsonObject checked;
        if(!adapter.isUndefined() && (!adapter.isObject() || !desktop::decode_numeric_node_adapter(QJsonDocument(adapter.toObject()).toJson(QJsonDocument::Compact),checked))){set_status(ui::text("Invalid network source settings"));return false;}
        if(!adapter.isUndefined())nodeAdapters[QString::number(node.value("id").toInteger())]=checked;
    }
    auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    domain::NumericProcessingGraph prepared;
    if(!numeric || !numeric->prepare_numeric_import(incoming,nodeAdapters,prepared,&error)){set_status(ui::text("Not applied: ")+error);return false;}
    NumericEditorGraphModel candidate(numeric->dataModelRegistry());
    if(!candidate.set_numeric_graph(prepared,&error,nodeAdapters)){set_status(ui::text("Not applied: ")+error);return false;}
    // Preserve visual/unknown fields while replacing only the prepared typed
    // snapshots. Saved adapter bindings and native geometry form one import.
    if(!nodes.isEmpty() && !nodes.first().toObject().contains("kind")) {
        auto preparedNodes=nodes;
        for(int i=0;i<preparedNodes.size();++i){auto node=preparedNodes[i].toObject();auto data=node.value("internal-data").toObject();
            const auto typed=candidate.saveNode(QtNodes::NodeId(node.value("id").toInteger())).value("internal-data").toObject();
            data["numeric_processing"]=typed.value("numeric_processing");node["internal-data"]=data;preparedNodes[i]=node;}
        visual["nodes"]=preparedNodes;
    }
    if(!nodes.isEmpty() && nodes.first().toObject().contains("kind")){
        visual=candidate.save();auto visualNodes=visual.value("nodes").toArray();
        QMap<int,QJsonObject> positions;
        if(active)for(const auto value:this->document().value("nodes").toArray()){const auto node=value.toObject();positions[node.value("id").toInt()]=node.value("position").toObject();}
        QMap<int,QJsonObject> presentations,adapters;
        for(const auto value:nodes){const auto node=value.toObject();presentations[node.value("id").toInt()]=node.value("presentation").toObject();adapters[node.value("id").toInt()]=node.value("adapter").toObject();if(node.value("position").isObject())positions[node.value("id").toInt()]=node.value("position").toObject();}
        for(int i=0;i<visualNodes.size();++i){auto node=visualNodes[i].toObject();const auto id=node.value("id").toInt();const auto position=positions.value(id);if(!position.isEmpty())node["position"]=position;auto data=node.value("internal-data").toObject();data["presentation"]=presentations.value(id);data["adapter"]=adapters.value(id);node["internal-data"]=data;visualNodes[i]=node;}
        visual["nodes"]=visualNodes;
        if(positions.isEmpty())visual["layout_pending"]=true;
    }
    if (!switch_device) flush_presentation();
    ++_document_generation;_live_apply_pending=false;
    _initial_layout_pending=false;

    _document_template = visual;
    inspect_node(QtNodes::InvalidNodeId);
    _apply_request = 0;
    _save_timer.stop(); _apply_timer.stop(); _loading=true;
    auto &graph=*_application.state().data_flow_graph;
    {
        const QSignalBlocker graph_blocker(&graph),scene_blocker(_scene);
        const auto ids=graph.allNodeIds(); for (auto id : ids) graph.deleteNode(id);
        auto visible=editable_document(_document_template,_unsupported_document);
        // NumericEditorGraphModel restores the preflighted graph in strict
        // import mode; interactive adaptation is only for newly drawn wires.
        graph.load(visible);
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
        _document_template.remove("layout_pending");
        _initial_layout_pending=true;
        _initial_layout_fit=view.isEmpty();
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
    clear_previews();
    if (active || switch_device) {
        _last_adapter_metadata=graph_adapter_metadata();
        _application.state().editor_draft_modified = false;
        _last_processing_document = processing_document(graph.save());
    } else changed();
    if(switch_device && !_unsupported_document){_live_apply_pending=true;_apply_timer.start();}
    sync_preview_subscription();
    if(_unsupported_document) set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
    schedule_initial_layout();
    emit execution_settings_changed();
    return true;
}
bool SchemeEditorWidget::load_preset(const QString &id) {
    if (_application.graph_request_pending()) return false;
    for (const auto &value : _application.state().file_configs.ui.pipeline_presets) {
        const auto preset=value.toObject();
        if (preset.value("id").toString()!=id || preset.value("version").toInt()!=1) continue;
        const auto presentation=preset.value("presentation");
        QJsonObject checkedPresentation;
        if(!presentation.isUndefined() && (!presentation.isObject() ||
            !desktop::decode_numeric_preset_presentation(QJsonDocument(presentation.toObject()).toJson(QJsonDocument::Compact),checkedPresentation)))return false;
        const auto document=preset.value("graph").toObject();
        if (!document.value("nodes").isArray() || !document.value("connections").isArray()) return false;
        QSet<int> ids;
        for (const auto &value : document.value("nodes").toArray()) {
            const auto node=value.toObject(); const int node_id=node.value("id").toInt(-1);
            if (node_id<0 || ids.contains(node_id)) return false;
            ids.insert(node_id);
        }
        if(!load_document(document,preset.value("view").toObject()))return false;
        emit preset_presentation_requested(checkedPresentation);
        return true;
    }
    return false;
}
void SchemeEditorWidget::refresh_capture_status() {
    core::perf::Scope profile(core::perf::Stage::CaptureStatus);
    if(!_capture_status)return;
    const char* label=nullptr;
    switch(pipeline::State(_capture_state)) {
    case pipeline::State::Starting:label="Capture starting";break;
    case pipeline::State::Running:label="Audio capture: receiving";break;
    case pipeline::State::Recovering:label="Capture recovering";break;
    case pipeline::State::Failed:label="Capture failed";break;
    case pipeline::State::Stopped:{
        label="Audio capture: inactive";
        const auto& active=_application.applied_numeric_graph();
        if(active){
            const bool usesCapture=std::any_of(active->nodes.begin(),active->nodes.end(),[&](const auto& n){
                return n.kind=="AudioSource" && std::any_of(active->edges.begin(),active->edges.end(),[&](const auto& edge){return edge.from==n.id;});});
            if(!usesCapture)label="Audio input: not used by this graph";
        }
        break;
    }
    }
    const auto text=label?ui::text(label):QString{};
    _capture_status->setText(text);
    _capture_status->setToolTip(ui::text("Audio input status. Processing continues independently; this indicator is not a start/stop control.")+(_capture_error.isEmpty()?QString{}:QStringLiteral("\n")+_capture_error));
    _capture_status->setVisible(!text.isEmpty());
}
void SchemeEditorWidget::apply() {
    _apply_timer.stop();
    if(_application.graph_request_pending()){_apply_timer.start();return;}
    domain::NumericProcessingGraph graph;QString error;
    if(!numeric_draft(graph,&error)){set_status(ui::text("Not applied: ")+error);return;}
    _application.state().editor_draft_modified=true;
    emit numeric_draft_changed();
    set_status({});
    _apply_request=_application.apply_pipeline_graph(document());
}
void SchemeEditorWidget::wire_source_model(QtNodes::NodeId id) {
    if(auto *node=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)){
        connect(node,&AudioNodeModel::numeric_edit_rejected,this,[this](const QString& error){set_status(ui::text("Not applied: ")+error);});
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
    domain::NumericProcessingGraph graph;
    if (!owned_processing_document(document,graph,&error)) {
        set_status(ui::text("Active Data Flow unavailable: ") + error);
        return;
    }
    if(load_document(document,{},true))set_status({});
}

SchemeEditorWidget::~SchemeEditorWidget() {
    _application.set_editor_numeric_endpoints({});
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
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());numeric && numeric->atomic_edit_in_progress())return;
    if (_loading) return;
    _application.state().editor_draft_modified = true;
    _save_timer.start();
    const auto processing = processing_document(_application.state().data_flow_graph->save());
    const auto adapters=graph_adapter_metadata();const bool adapters_changed=adapters!=_last_adapter_metadata;_last_adapter_metadata=adapters;
    if (processing != _last_processing_document) {
        _live_apply_pending=true;
        _last_processing_document = processing;
        _apply_timer.stop();
        _apply_request=0; // An acknowledgement of an older processing draft cannot label this one active.
        for (const auto id : _application.state().data_flow_graph->allNodeIds())
            if (auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) model->clear_preview();
        _preview_times.clear();
        sync_preview_subscription();
        QString error;
        domain::NumericProcessingGraph draft;const bool valid=numeric_draft(draft,&error);
        set_status(valid ? QString{} : ui::text("Not applied: ")+error);
        if(_unsupported_document)
            set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
        else if(valid) _apply_timer.start();
    }
    if(adapters_changed){_apply_request=0;sync_preview_subscription();if(!_unsupported_document){_live_apply_pending=true;_apply_timer.start();}}
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
    if(_application.state().editor_draft_modified)emit numeric_draft_changed();
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
            const auto category=model ? numeric_editor::category(model->name()) : QString();
            return category=="Sources" ? 0 : category=="Processors" ? 1 : 2;
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
