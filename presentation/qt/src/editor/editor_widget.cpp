#include "barelief/ui_text.h"
#include "core/performance.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "core_app/wheel_input.h"
#include "core_app/touchpad_input.h"
#include <QNativeGestureEvent>
#include <utility>

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
#include <QScopedValueRollback>
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
#include <vector>
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

static bool add_clipboard_node(const QJsonValue& value,NumericEditorGraphModel& candidate) {
    const auto node=value.toObject();const auto id=node.value("id").toInteger(-1);
    const auto kind=node.value("internal-data").toObject().value("model-name").toString();
    if(!value.isObject() || id<0 || std::uint64_t(id)>=std::numeric_limits<QtNodes::NodeId>::max() ||
       node.value("id").toDouble()!=double(id) || candidate.nodeExists(QtNodes::NodeId(id)) ||
       !numeric_editor::catalog().contains(kind))return false;
    const auto position=node.value("position").toObject();
    if(!position.value("x").isDouble() || !position.value("y").isDouble() ||
       !std::isfinite(position.value("x").toDouble()) || !std::isfinite(position.value("y").toDouble()))return false;
    const auto presentation=node.value("internal-data").toObject().value("presentation");
    if((!presentation.isUndefined() && !presentation.isObject()) ||
       !AudioNodeModel::valid_presentation(presentation.toObject()))return false;
    candidate.loadNode(node);
    return true;
}
static bool valid_group_members(const QJsonArray& members,NumericEditorGraphModel& candidate,
                                QSet<qint64>& groupedNodes) {
    for(const auto member:members) {
        const auto node=member.toInteger(-1);
        if(!member.isDouble() || node<0 ||
           std::uint64_t(node)>=std::numeric_limits<QtNodes::NodeId>::max() ||
           member.toDouble()!=double(node) || !candidate.nodeExists(QtNodes::NodeId(node)) ||
           groupedNodes.contains(node))return false;
        groupedNodes.insert(node);
    }
    return true;
}
static bool valid_clipboard_groups(const QJsonValue& groups,const QJsonArray& nodes,
                                   NumericEditorGraphModel& candidate) {
    if(!groups.isUndefined() && (!groups.isArray() || groups.toArray().size()>nodes.size()))return false;
    QSet<qint64> groupIds,groupedNodes;
    const auto groups_array=groups.toArray();
    for(const auto value:groups_array) {
        const auto group=value.toObject();const auto id=group.value("id").toInteger(-1);
        if(!value.isObject() || !group.value("id").isDouble() || id<0 ||
           group.value("id").toDouble()!=double(id) || groupIds.contains(id) ||
           !group.value("name").isString() || group.value("name").toString().size()>1024 ||
           !group.value("nodes").isArray())return false;
        groupIds.insert(id);
        if(!valid_group_members(group.value("nodes").toArray(),candidate,groupedNodes))return false;
    }
    return true;
}
static bool valid_clipboard_connection(const QJsonValue& value) {
    if(!value.isObject())return false;
    for(const auto* key:{"outNodeId","inNodeId","outPortIndex","inPortIndex"}) {
        const auto field=value.toObject().value(key);const auto id=field.toInteger(-1);
        if(!field.isDouble() || id<0 || std::uint64_t(id)>=std::numeric_limits<QtNodes::NodeId>::max() ||
           field.toDouble()!=double(id))return false;
    }
    const auto policy=value.toObject().value("conversion");
    return policy.isUndefined() || (policy.isDouble() && policy.toInt(-1)>=0 &&
                                    policy.toInt(-1)<=3 && policy.toDouble()==policy.toInt(-1));
}
static bool copy_clipboard_connections(const QJsonArray& source,NumericEditorGraphModel& candidate,
                                       QJsonArray& connections) {
    for(const auto value:source) {
        if(!valid_clipboard_connection(value))return false;
        const auto id=QtNodes::fromJson(value.toObject());
        if(!candidate.nodeExists(id.outNodeId) || !candidate.nodeExists(id.inNodeId))continue;
        if(candidate.connectionExists(id))continue;
        if(!candidate.connectionPossible(id))return false;
        candidate.addConnection(id);connections.append(value);
    }
    return true;
}
static bool valid_clipboard_document(const QJsonObject& document) {
    const auto nodes=document.value("nodes").toArray();
    return document.value("nodes").isArray() && !nodes.isEmpty() &&
        nodes.size()<=int(numeric_editor::limits().max_nodes) &&
        document.value("connections").isArray() &&
        document.value("connections").toArray().size()<=int(numeric_editor::limits().max_edges);
}
static std::shared_ptr<QtNodes::NodeDelegateModelRegistry> clipboard_registry() {
    auto registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    const auto kinds=numeric_editor::catalog();
    for(const auto& kind:kinds)registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);});
    return registry;
}
static void replace_clipboard_connections(QJsonObject document,const QJsonArray& connections) {
    document["connections"]=connections;
    const auto corrected=QJsonDocument(document).toJson(QJsonDocument::Compact);
    auto *result=new QMimeData;
    result->setData("application/qt-nodes-graph",corrected);
    result->setText(QString::fromUtf8(corrected));
    QApplication::clipboard()->setMimeData(result);
}
static bool trim_clipboard_connections() {
    const auto *mime=QApplication::clipboard()->mimeData();
    if(!mime) return false;
    const auto bytes=mime->hasFormat("application/qt-nodes-graph")
        ? mime->data("application/qt-nodes-graph") : mime->text().toUtf8();
      if(bytes.size()>16*1024*1024)return false;
    auto document=QJsonDocument::fromJson(bytes).object();
    if(!valid_clipboard_document(document))return false;
    const auto nodes=document.value("nodes").toArray();
    NumericEditorGraphModel candidate(clipboard_registry());
    for(const auto value:nodes)if(!add_clipboard_node(value,candidate))return false;
    domain::NumericProcessingGraph checked;if(!candidate.export_numeric_graph(checked,nullptr,false))return false;
    if(!valid_clipboard_groups(document.value("groups"),nodes,candidate))return false;
    QJsonArray connections;
    const auto document_connections=document.value("connections").toArray();
    if(!copy_clipboard_connections(document_connections,candidate,connections))return false;
    // QtNodes Copy includes incident edges to unselected nodes. Its Paste then
    // remaps absent endpoints to ID 0, producing invalid links/failed insertion.
    if(connections==document.value("connections").toArray()) return true;
    replace_clipboard_connections(document,connections);
    return true;
}

class LocalizedDataFlowScene final : public DataFlowGraphicsScene {
public:
    using DataFlowGraphicsScene::DataFlowGraphicsScene;
    std::function<void(int)> focus_graph;
    std::function<void(int,int)> open_graph;
    std::function<void(int,QMenu *)> graph_menu;
    void add_connection_policies(QMenu* menu,NumericEditorGraphModel* graph,QtNodes::ConnectionId edge) {
        auto* policies=menu->addMenu(ui::text("Connection conversion"));
        auto* status=menu->addAction(graph->connection_status(edge));status->setEnabled(false);
        for(std::uint8_t mode=0;mode<4;++mode){
            auto* action=policies->addAction(numeric_editor::conversion_policy_caption(mode));
            action->setCheckable(true);action->setChecked(graph->connection_policy(edge)==mode);
            connect(action,&QAction::triggered,this,[graph,edge,mode]{graph->edit_connection_policy(edge,mode);});
        }
    }
    void add_connection_details(QMenu* menu,QPointF scene_position) {
        const auto hit_items=items(scene_position);
        for(auto* item:hit_items)if(auto* wire=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            auto* graph=dynamic_cast<NumericEditorGraphModel*>(&graphModel());if(!graph)break;
            const auto edge=wire->connectionId();
            auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);
            if(!from || !to)break;
            auto* detail=menu->addAction(from->caption()+" → "+to->caption()+"\n"+from->portCaption(QtNodes::PortType::Out,edge.outPortIndex)+" → "+to->portCaption(QtNodes::PortType::In,edge.inPortIndex));detail->setEnabled(false);
            add_connection_policies(menu,graph,edge);
            break;
        }
    }
    void bind_copy_actions(QMenu* menu) {
        const auto actions=menu->actions();
        for(auto *action:actions) {
            if(action->text()=="Copy") ui::bind(action,"text","Copy");
            else if(action->text()=="Cut") ui::bind(action,"text","Cut");
            connect(action,&QAction::triggered,this,[] { trim_clipboard_connections(); });
        }
    }
    void add_paste_delete_actions(QMenu* menu,QPointF scene_position) {
        auto *paste=menu->addAction(ui::text("Paste")); paste->setShortcut(QKeySequence::Paste);
        connect(paste,&QAction::triggered,this,[this,scene_position] {
            if(!trim_clipboard_connections())return;
            undoStack().push(new QtNodes::PasteCommand(this,scene_position));
        });
        auto *remove=menu->addAction(ui::text("Delete selection"));
        remove->setObjectName("deleteGraphSelection"); remove->setShortcut(QKeySequence(Qt::Key_Delete));
        connect(remove,&QAction::triggered,this,[this] { undoStack().push(new QtNodes::DeleteCommand(this)); });
    }
    void add_graph_navigation(QMenu* menu,int id) {
        if(graph_menu) {graph_menu(id,menu);return;}
        auto *focus=menu->addAction(ui::text("Focus graph")); focus->setObjectName("focusGraph");
        connect(focus,&QAction::triggered,this,[this,id] {if(focus_graph)focus_graph(id);});
        add_graph_placement_actions(menu,id);
    }
    void add_graph_placement_actions(QMenu* menu,int id) {
        const std::array<const char*,3> labels{"Open in separate window","Open in new tab","Open beside current graph"};
        for(int placement=0;placement<3;++placement) {
            auto *action=menu->addAction(ui::text(labels[placement]));
            action->setObjectName(QStringLiteral("openGraphPlacement%1").arg(placement));
            if(placement<2) {
                action->setShortcut(QKeySequence(placement==0 ? "Alt+Shift+G" : "Alt+G"));
                action->setShortcutVisibleInContextMenu(true);
            }
            connect(action,&QAction::triggered,this,[this,id,placement] {if(open_graph)open_graph(id,placement);});
        }
    }
    QtNodes::NodeGraphicsObject* node_at(QPointF scene_position) {
        const auto hit_items=items(scene_position);
        for(auto* item:hit_items)
            for(auto* parent=item;parent;parent=parent->parentItem())
                if(auto* node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(parent))return node;
        return nullptr;
    }
    void add_fourier_dc_action(QMenu* menu,AudioNodeModel* model) {
        if(!model || (model->name()!="Fourier" && model->name()!="RealFourier"))return;
        menu->addSeparator();auto* dc=menu->addAction(ui::text("Show DC outputs"));dc->setObjectName("showFourierDCOutputs");
        dc->setCheckable(true);dc->setChecked(model->dc_outputs_shown());
        connect(dc,&QAction::triggered,model,[model](bool shown){model->edit_dc_outputs_shown(shown);});
    }
    void add_node_actions(QMenu* menu,QPointF scene_position) {
        auto* node=node_at(scene_position);
        if(!node)return;
        auto &graph=static_cast<DataFlowGraphModel &>(graphModel());
        auto *model=graph.delegateModel<AudioNodeModel>(node->nodeId());
        add_fourier_dc_action(menu,model);
        if(model && model->name()=="InputSelector"){menu->addSeparator();model->populate_selector_menu(menu);return;}
        if(model && model->name()=="ArrayMultiply"){menu->addSeparator();model->populate_selector_menu(menu);}
        if(graph_available(model)){menu->addSeparator();add_graph_navigation(menu,int(node->nodeId()));}
    }
    QMenu *createStdMenu(QPointF scene_position) {
        auto *menu=DataFlowGraphicsScene::createStdMenu(scene_position);
        add_connection_details(menu,scene_position);
        bind_copy_actions(menu);
        add_paste_delete_actions(menu,scene_position);
        add_node_actions(menu,scene_position);
        return menu;
    }
    QMenu *createSceneMenu(QPointF scene_position) override {
        if(!items(scene_position).empty()) return createStdMenu(scene_position);
        return create_catalog_menu(scene_position);
    }
    QMenu *create_catalog_menu(QPointF scene_position) {
        auto* menu=new QMenu;auto* content=new QWidget(menu);auto* layout=new QVBoxLayout(content);
        auto* search=new QLineEdit(content);ui::bind(search,"placeholderText","Filter");layout->addWidget(search);
        auto* tree=new QTreeWidget(content);tree->setObjectName("numericContextCatalog");tree->setHeaderHidden(true);layout->addWidget(tree);
        numeric_editor::populate_catalog(tree);
        bind_catalog_menu(menu,search,tree,scene_position);
        auto* action=new QWidgetAction(menu);action->setDefaultWidget(content);menu->addAction(action);
        return menu;
    }
    void bind_catalog_menu(QMenu* menu,QLineEdit* search,QTreeWidget* tree,QPointF scene_position) {
        connect(&ui::Language::instance(),&ui::Language::changed,menu,[tree]{numeric_editor::populate_catalog(tree);});
        connect(search,&QLineEdit::textChanged,tree,[tree](const QString& query){numeric_editor::filter_catalog(tree,query);});
        connect(tree,&QTreeWidget::itemDoubleClicked,this,[this,menu,scene_position](QTreeWidgetItem* item){const auto kind=item->data(0,Qt::UserRole).toString();if(kind.isEmpty())return;undoStack().push(new QtNodes::CreateCommand(this,kind,scene_position));menu->close();});
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
        bind_navigation_input();
    }
    void bind_navigation_input() {
        for(auto *scroll:{horizontalScrollBar(),verticalScrollBar()})
            connect(scroll,&QScrollBar::valueChanged,this,[this] { if(!_restoring) remember_camera(); });
        _pan_timer.setInterval(16);
        _pan_timer.setTimerType(Qt::PreciseTimer);
        connect(&_pan_timer,&QTimer::timeout,this,[this] {advance_keyboard_pan();});
        connect(qApp,&QApplication::focusChanged,this,[this](QWidget *,QWidget *) {
            if(!hasFocus() || editing_control_focused()) stop_keyboard_pan();
        });
    }
    std::function<void()> on_interaction;
    std::function<void()> on_connection_rejected;
    std::function<void()> synchronize_positions;
    std::function<void(QtNodes::NodeId,bool)> on_graph_clicked;
    void advance_keyboard_pan() {
        if(!isVisible() || !hasFocus() || editing_control_focused() ||
           QApplication::keyboardModifiers()!=Qt::NoModifier) {
            stop_keyboard_pan();return;
        }
        const double seconds=std::min(_pan_clock.nsecsElapsed()/1e9,0.05);
        _pan_clock.restart();
        QPointF direction;
        for(auto key:std::as_const(_held_pan_keys))direction+=pan_direction(key);
        const double length=std::hypot(direction.x(),direction.y());
        if(length>0)pan_pixels(direction*(360.0*(_navigation_speed/3.0)*seconds/length));
    }
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
        const auto graph_actions=actions();
        for(auto *action:graph_actions)configure_graph_shortcut(action);
    }
    void configure_graph_shortcut(QAction* action) {
            action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
            action->setAutoRepeat(false);
            clear_shared_history_shortcut(action);
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
    void clear_shared_history_shortcut(QAction* action) {
        const auto shortcuts=action->shortcuts();
        if(shortcuts.contains(QKeySequence(QKeySequence::Undo)) ||
           shortcuts.contains(QKeySequence(QKeySequence::Redo)))
            action->setShortcuts({}); // Shared File menu owns history shortcuts.
    }
    void remember_camera() {
        if (!_restoring && isVisible()) _center=mapToScene(viewport()->rect().center());
    }
    QPointF camera_center() const { return _center; }
    QRectF camera_bounds(double scale,QPointF center,bool restore) {
        auto bounds=nodeScene()->itemsBoundingRect();
        if(bounds.isEmpty())bounds=QRectF(-100,-100,200,200);
        const auto half=QSizeF(viewport()->size())/(2*scale);
        // Preserve the reachable viewport anchor while panning.
        if(restore)_camera_anchor=center;
        bounds=bounds.united(QRectF(_camera_anchor-QPointF(half.width(),half.height()),half*2));
        const auto size=QSizeF(std::max(bounds.width(),2*half.width()),std::max(bounds.height(),2*half.height()));
        bounds=QRectF(bounds.center()-QPointF(size.width()/2,size.height()/2),size);
        const double margin=160/scale;
        bounds.adjust(-margin,-margin,margin,margin);
        return bounds;
    }
    void camera(double scale, QPointF center, bool restore = true) {
        _restoring = true;
        // setupScale skips equal scales; reset also removes old zoom translations.
        const bool scaleChangedNow=std::abs(scale-getScale())>1e-9;
        if(scaleChangedNow) { QTransform transform; transform.scale(scale,scale); setTransform(transform); }
        const auto bounds=camera_bounds(scale,center,restore);
        const auto half=QSizeF(viewport()->size())/(2*scale);
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
    bool handle_native_gesture(QNativeGestureEvent* gesture) {
        if(gesture->gestureType()==Qt::PanNativeGesture) {
            pan_pixels(-gesture->delta());gesture->accept();return true;
        }
        if(gesture->gestureType()!=Qt::ZoomNativeGesture)return false;
        const double next=std::clamp(getScale()*(1+gesture->value()),0.2,3.0);
        const auto anchor=mapToScene(gesture->position().toPoint());
        const auto offset=(gesture->position()-QPointF(viewport()->rect().center()))/next;
        camera(next,anchor-offset);
        gesture->accept();if(on_interaction)on_interaction();
        return true;
    }
    bool viewportEvent(QEvent *event) override {
        if(event->type()==QEvent::NativeGesture &&
           handle_native_gesture(static_cast<QNativeGestureEvent *>(event)))return true;
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
            const auto graph_actions=actions();
            for(auto *action:graph_actions) if(action->shortcuts().contains(key)) {
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
    QtNodes::NodeId pressed_node_at(QPoint point) {
        const auto hit_items=items(point);
        for(auto *item:hit_items) {
            for(auto *parent=item;parent;parent=parent->parentItem())
                if(auto *node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject *>(parent))return node->nodeId();
        }
        return QtNodes::InvalidNodeId;
    }
    bool handle_node_control_press(QMouseEvent* event) {
        if(event->button()!=Qt::LeftButton || event->modifiers()!=Qt::NoModifier ||
           _pressed_node==QtNodes::InvalidNodeId)return false;
        auto* node=nodeScene()->nodeGraphicsObject(_pressed_node);
        const auto point=node->mapFromScene(mapToScene(event->position().toPoint()));
        auto& geometry=static_cast<EditorNodeGeometry&>(nodeScene()->nodeGeometry());
        if(geometry.bypassRect(_pressed_node).contains(point)) {
            toggle_node_bypass(geometry,event);
            return true;
        }
        return select_node_input(geometry,point,event);
    }
    void toggle_node_bypass(EditorNodeGeometry& geometry,QMouseEvent* event) {
        auto* parameters=geometry.model(_pressed_node)->parameter_model();
        parameters->setBypassed(!parameters->bypassed());event->accept();
    }
    bool select_node_input(EditorNodeGeometry& geometry,QPointF point,QMouseEvent* event) {
        const auto port=geometry.selectorButtonHit(_pressed_node,point);
        if(port==QtNodes::InvalidPortIndex)return false;
        auto* model=geometry.model(_pressed_node);
        const auto channels=domain::data_outputs(*model->numeric_node()).size();
        const auto input=model->port_id(QtNodes::PortType::In,port);
        model->select_input(channels>1 ? domain::PortId(input/channels) : input);
        event->accept();return true;
    }
    bool handle_reorder_press(QMouseEvent* event) {
        if(event->button()!=Qt::LeftButton || !event->modifiers().testFlag(Qt::AltModifier) ||
           _pressed_node==QtNodes::InvalidNodeId)return false;
        auto* node=nodeScene()->nodeGraphicsObject(_pressed_node);
        const auto point=node->mapFromScene(mapToScene(event->position().toPoint()));
        for(auto side:{QtNodes::PortType::In,QtNodes::PortType::Out}) {
            const auto port=nodeScene()->nodeGeometry().checkPortHit(_pressed_node,side,point);
            if(port==QtNodes::InvalidPortIndex)continue;
            _reorder_node=_pressed_node;_reorder_side=side;_reorder_port=port;
            event->accept();return true;
        }
        return false;
    }
    bool begin_middle_pan(QMouseEvent* event) {
        if(event->button()!=Qt::MiddleButton || event->buttons()!=Qt::MiddleButton ||
           event->modifiers()!=Qt::NoModifier || editing_control_focused())return false;
        _middle_panning=true;_middle_last=event->position().toPoint();
        _previous_cursor=viewport()->cursor();viewport()->setCursor(Qt::ClosedHandCursor);
        setFocus(Qt::MouseFocusReason);event->accept();return true;
    }
    void mousePressEvent(QMouseEvent *event) override {
        QToolTip::hideText();_hover_wire=nullptr;
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        _press_pointer=_last_pointer; _pressed_node=QtNodes::InvalidNodeId;
        _press_alt=event->modifiers().testFlag(Qt::AltModifier);
        if(event->button()==Qt::LeftButton)_pressed_node=pressed_node_at(_last_pointer);
        if(handle_node_control_press(event))return;
        stop_keyboard_pan();
        if(handle_reorder_press(event) || begin_middle_pan(event))return;
        GraphicsView::mousePressEvent(event);
    }
    QtNodes::ConnectionGraphicsObject* hovered_connection(const QPoint& position) const {
        const auto hit_items=items(position);
        for(auto* item:hit_items)
            if(auto* wire=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item))return wire;
        return nullptr;
    }
    void update_wire_hover(QtNodes::ConnectionGraphicsObject* hovered,QMouseEvent* event) {
        if(hovered==_hover_wire)return;
        _hover_wire=hovered;
        if(!hovered){QToolTip::hideText();return;}
        auto* model=dynamic_cast<NumericEditorGraphModel*>(&hovered->graphModel());
        auto tooltip=ui::text("Click to inspect conversion");
        if(model && model->connection_visual_state(hovered->connectionId())>=2)
            tooltip+="\n"+model->connection_status(hovered->connectionId())+"\n"+
                model->connection_failure(hovered->connectionId());
        QToolTip::showText(event->globalPosition().toPoint(),tooltip,viewport());
    }
    bool continue_middle_pan(QMouseEvent* event) {
        if(!_middle_panning)return false;
        if(!(event->buttons() & Qt::MiddleButton)){
            finish_middle_pan();GraphicsView::mouseMoveEvent(event);return true;
        }
        const auto point=event->position().toPoint();
        pan_pixels(_middle_last-point);_middle_last=point;
        event->accept();return true;
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        QtNodes::ConnectionGraphicsObject* hovered=nullptr;
        if(event->buttons()==Qt::NoButton)hovered=hovered_connection(event->position().toPoint());
        update_wire_hover(hovered,event);
        if(_reorder_node!=QtNodes::InvalidNodeId){event->accept();return;}
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        if(continue_middle_pan(event))return;
        GraphicsView::mouseMoveEvent(event);
    }
    void leaveEvent(QEvent* event) override {QToolTip::hideText();_hover_wire=nullptr;GraphicsView::leaveEvent(event);}
    void draw_grid(QPainter* painter,const QRectF& rect,const QColor& background,
                   const QColor& foreground,double spacing,double amount) {
        const auto color=QColor::fromRgbF(
            background.redF()*(1-amount)+foreground.redF()*amount,
            background.greenF()*(1-amount)+foreground.greenF()*amount,
            background.blueF()*(1-amount)+foreground.blueF()*amount);
        painter->setPen(QPen(color,0));
        for(double x=std::floor(rect.left()/spacing)*spacing;x<=rect.right();x+=spacing)
            painter->drawLine(QLineF(x,rect.top(),x,rect.bottom()));
        for(double y=std::floor(rect.top()/spacing)*spacing;y<=rect.bottom();y+=spacing)
            painter->drawLine(QLineF(rect.left(),y,rect.right(),y));
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
        draw_grid(painter,rect,background,foreground,step,0.06);
        draw_grid(painter,rect,background,foreground,step*5,0.10);
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
    QtNodes::PortIndex nearest_reorder_port(const std::vector<QtNodes::PortIndex>& order,QPointF point) {
        auto closest=_reorder_port;
        double distance=std::numeric_limits<double>::max();
        for(auto port:order) {
            const auto delta=std::abs(nodeScene()->nodeGeometry().portPosition(
                _reorder_node,_reorder_side,port).y()-point.y());
            if(delta<distance){distance=delta;closest=port;}
        }
        return closest;
    }
    void apply_port_order(NumericEditorGraphModel& graph,QtNodes::NodeGraphicsObject& node,
                          AudioNodeModel& model,const std::vector<QtNodes::PortIndex>& order) {
        QJsonArray ids;
        for(auto port:order)ids.append(int(model.port_id(_reorder_side,port)));
        auto presentation=model.presentation();
        presentation[_reorder_side==QtNodes::PortType::In ? "input_port_order" : "output_port_order"]=ids;
        graph.edit_presentation(_reorder_node,presentation);
        node.setGeometryChanged();node.updateQWidgetEmbedPos();node.moveConnections();node.update();
    }
    void commit_port_reorder() {
        auto* graph=dynamic_cast<NumericEditorGraphModel*>(&nodeScene()->graphModel());
        auto* node=nodeScene()->nodeGraphicsObject(_reorder_node);
        auto* model=graph ? graph->delegateModel<AudioNodeModel>(_reorder_node) : nullptr;
        if(!node || !model)return;
        auto order=model->visual_port_order(_reorder_side);
        const auto point=node->mapFromScene(mapToScene(_last_pointer));
        const auto closest=nearest_reorder_port(order,point);
        const auto target=std::find(order.begin(),order.end(),closest)-order.begin();
        order.erase(std::find(order.begin(),order.end(),_reorder_port));
        order.insert(order.begin()+std::min<std::size_t>(target,order.size()),_reorder_port);
        apply_port_order(*graph,*node,*model,order);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        _last_pointer=event->position().toPoint(); _pointer_seen=true;
        if(_reorder_node!=QtNodes::InvalidNodeId){
            finish_reorder_release(event);return;
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
    void finish_reorder_release(QMouseEvent* event) {
        commit_port_reorder();
        _reorder_node=QtNodes::InvalidNodeId;
        _pressed_node=QtNodes::InvalidNodeId;
        event->accept();
        if(on_interaction)on_interaction();
    }
    bool handle_wheel_pan(QWheelEvent* event,QPointF scroll) {
        const bool touchpad=desktop_input::is_touchpad(*event);
        if(!touchpad && event->modifiers().testFlag(Qt::ControlModifier)) {
            pan_pixels({0,-scroll.y()});event->accept();return true;
        }
        if(!touchpad && event->modifiers().testFlag(Qt::AltModifier)) {
            pan_pixels({-(scroll.y()!=0 ? scroll.y() : scroll.x()),0});event->accept();return true;
        }
        if(touchpad && !desktop_input::is_pinch(*event)) {
            pan_pixels(-scroll);event->accept();return true;
        }
        return false;
    }
    void wheelEvent(QWheelEvent *event) override {
        if(event->phase()==Qt::ScrollMomentum) { event->accept(); return; }
        const auto scroll=event->pixelDelta().isNull() ? QPointF(event->angleDelta())/3.0 : QPointF(event->pixelDelta());
        if(handle_wheel_pan(event,scroll))return;
        const double delta=event->pixelDelta().isNull() ? event->angleDelta().y()/120.0 : event->pixelDelta().y()/40.0;
        if (delta==0) { event->ignore(); return; }
        zoom_from_wheel(event,delta);
    }
    void zoom_from_wheel(QWheelEvent* event,double delta) {
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
        const auto hit_items=items(pos);
        for(auto *item:hit_items) if(auto *proxy=qgraphicsitem_cast<QGraphicsProxyWidget *>(item)) {
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
static QJsonArray editable_nodes(const QJsonObject& document,bool& unsupported,QSet<int>& ids) {
    QJsonArray nodes;
    const auto document_nodes=document.value("nodes").toArray();
    for(const auto &value:document_nodes) {
        const auto node=value.toObject();
        if(!numeric_editor::catalog().contains(node.value("internal-data").toObject().value("model-name").toString())) {
            unsupported=true; continue;
        }
        nodes.append(node); ids.insert(node.value("id").toInt());
    }
    return nodes;
}
static QJsonArray editable_connections(const QJsonObject& document,const QSet<int>& ids) {
    QJsonArray connections;
    const auto document_connections=document.value("connections").toArray();
    for(const auto &value:document_connections) {
        const auto edge=value.toObject();
        if(ids.contains(edge.value("outNodeId").toInt()) && ids.contains(edge.value("inNodeId").toInt())) connections.append(edge);
    }
    return connections;
}
static QJsonObject editable_document(const QJsonObject &document, bool &unsupported) {
    auto visible=document;
    const auto version=document.value("version").toInt();
    unsupported=!document.isEmpty() && (version!=5 || document.value("processing_version").toInt()!=4);
    QSet<int> ids;
    const auto nodes=editable_nodes(document,unsupported,ids);
    const auto connections=editable_connections(document,ids);
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
    initialize_document();configure_scene();configure_view();
    setup_editor_chrome();
    bind_view_interactions();
    initialize_preview_ui();
}
void SchemeEditorWidget::setup_editor_chrome() {
    auto *layout = new QVBoxLayout(this);
    auto* bar=create_toolbar(layout);setup_presets();
    setup_processing_rate(bar);setup_zoom_control(bar);auto* fit=setup_center_control(bar);
    setup_splitter(layout);
    bind_scene_selection();
    setup_validation_status(bar);setup_capture_status(bar);bind_apply_timer();bind_save_timer();
    bind_graph_model();
    setup_editor_display(bar);
    setup_canvas_actions(bar,fit);
}

QHBoxLayout* SchemeEditorWidget::create_toolbar(QVBoxLayout* layout) {
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
    bar->addWidget(apply_button);
    connect(dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get()),&NumericEditorGraphModel::edit_rejected,this,[this](const QString& error){set_status(ui::text("Not applied: ")+error);});
    layout->setContentsMargins(3,3,3,3);layout->addWidget(_toolbar);return bar;
}
void SchemeEditorWidget::initialize_preview_ui() {
    initialize_preview_tracking();bind_preview_delivery();bind_application_feedback();restore_initial_camera();
}
QAction* SchemeEditorWidget::canvas_shortcut(const char* label,const char* id,const QKeySequence& key,void(SchemeEditorWidget::*handler)()) {
    const auto actions=_view->actions();
    for(auto* existing:actions) {auto shortcuts=existing->shortcuts();shortcuts.removeAll(key);existing->setShortcuts(shortcuts);}
    auto* action=new QAction(_view);action->setObjectName(id);ui::bind(action,"text",label);action->setShortcut(key);
    action->setShortcutContext(Qt::WidgetShortcut);action->setAutoRepeat(false);_view->addAction(action);connect(action,&QAction::triggered,this,handler);return action;
}
int SchemeEditorWidget::selected_graph() const {
    const auto selected=_scene->selectedItems();
    for(auto* item:selected)if(auto* node=qgraphicsitem_cast<QtNodes::NodeGraphicsObject*>(item)){
        auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(node->nodeId());if(graph_available(model))return int(node->nodeId());
    }
    return -1;
}
void SchemeEditorWidget::setup_graph_shortcut(const char* label,const char* id,const QKeySequence& key,int placement) {
    auto* action=new QAction(this);action->setObjectName(id);ui::bind(action,"text",label);action->setShortcut(key);
    action->setShortcutContext(Qt::WidgetWithChildrenShortcut);addAction(action);
    connect(action,&QAction::triggered,this,[this,placement]{open_selected_graph(placement);});
}
void SchemeEditorWidget::open_selected_graph(int placement) {
    if(const auto id=selected_graph();id>=0)emit graph_open_requested(id,placement);
}
void SchemeEditorWidget::setup_canvas_actions(QHBoxLayout* bar,QPushButton* fit) {
    connect(&_scene->undoStack(),&QUndoStack::indexChanged,this,&SchemeEditorWidget::changed);
    auto* arrange=ui::make<QPushButton>("Arrange",this);ui::compact_control(arrange);ui::bind(arrange,"toolTip","Arrange nodes from left to right (Alt+A)");
    arrange->setObjectName("arrangePipeline");bar->insertWidget(bar->indexOf(fit),arrange);connect(arrange,&QPushButton::clicked,this,&SchemeEditorWidget::arrange_nodes);
    _arrange_action=canvas_shortcut("Arrange Data Flow","arrangePipelineShortcut",QKeySequence(Qt::ALT|Qt::Key_A),&SchemeEditorWidget::arrange_nodes);
    _center_action=canvas_shortcut("Center Data Flow","fitPipelineShortcut",QKeySequence(Qt::ALT|Qt::Key_F),&SchemeEditorWidget::fit_nodes);
    setup_graph_shortcut("Open in new tab","showSelectedGraph",QKeySequence("Alt+G"),1);
    setup_graph_shortcut("Open in separate window","floatSelectedGraph",QKeySequence("Alt+Shift+G"),0);
}
void SchemeEditorWidget::bind_view_interactions() {
    bind_view_camera_saving();
    _view->viewport()->installEventFilter(this);
    static_cast<ProfiledGraphicsView *>(_view)->on_connection_rejected=[this]{set_status(ui::text("Connection rejected: incompatible or unsupported ports"));};
    static_cast<ProfiledGraphicsView *>(_view)->on_interaction = [this] { if (!_loading) {_initial_layout_pending=false;_save_timer.start();} };
    static_cast<ProfiledGraphicsView *>(_view)->synchronize_positions=[this] {synchronize_moved_positions();};
}
void SchemeEditorWidget::bind_view_camera_saving() {
    connect(_view, &GraphicsView::scaleChanged, this, [this] { if (!_loading) _save_timer.start(); });
    for (auto *scroll : {_view->horizontalScrollBar(), _view->verticalScrollBar()})
        connect(scroll, &QScrollBar::sliderMoved, this, [this] { if (!_loading) _save_timer.start(); });
}
void SchemeEditorWidget::synchronize_moved_positions() {
    if(_loading)return;
    auto& graph=*_application.state().data_flow_graph;
    std::map<QtNodes::NodeId,QPointF> moved;
    for(const auto id:graph.allNodeIds()) {
        auto* item=_scene->nodeGraphicsObject(id);
        if(item && item->pos()!=graph.nodeData(id,QtNodes::NodeRole::Position).toPointF())
            moved[id]=item->pos();
    }
    if(!moved.empty())_scene->undoStack().push(new ArrangeCommand(graph,std::move(moved),ui::text("Move nodes")));
}
void SchemeEditorWidget::setup_validation_status(QHBoxLayout* bar) {
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
}
void SchemeEditorWidget::setup_capture_status(QHBoxLayout* bar) {
    _capture_status=new QLabel(_toolbar);_capture_status->setObjectName("pipelineCaptureStatus");
    _capture_status->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Preferred);
    _capture_status->setTextInteractionFlags(Qt::NoTextInteraction);
    _capture_status->setTextFormat(Qt::PlainText);
    bar->addWidget(_capture_status);
    connect(&_application,&GuiPresenter::component_status_changed,this,[this](const QString& component,int state,const QString& error){
        if(component!="capture")return;
        _capture_state=state;_capture_error=error;refresh_capture_status();
    });
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_capture_status);
    connect(&_application,&GuiPresenter::active_graph_changed,this,[this]{refresh_capture_status();});
    const auto capture=_application.client().snapshot().statuses.value("capture");
    _capture_state=int(capture.state);_capture_error=capture.error;refresh_capture_status();

}
void SchemeEditorWidget::bind_apply_timer() {
    connect(this,&SchemeEditorWidget::status_message,_apply_button,&QPushButton::setToolTip);
    _apply_timer.setSingleShot(true);
    _apply_timer.setInterval(350);
    connect(&_apply_timer, &QTimer::timeout, this, [this] {
        apply();
    });
    connect(_apply_button, &QPushButton::clicked, this, &SchemeEditorWidget::apply);
    connect(&_application,&GuiPresenter::on_config_updated,this,&SchemeEditorWidget::sync_preferences);
    _auto_apply=_application.state().file_configs.ui.pipeline_auto_apply;
    sync_preferences();
}
void SchemeEditorWidget::bind_save_timer() {
    _last_processing_document = processing_document(_application.state().data_flow_graph->save());
    _last_adapter_metadata=graph_adapter_metadata();
    _save_timer.setSingleShot(true); _save_timer.setInterval(350);
    connect(&_save_timer, &QTimer::timeout, this, [this] {
        save_view();
        if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());numeric && numeric->has_numeric_nodes()){emit numeric_draft_changed();return;}
        if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    });
}
void SchemeEditorWidget::setup_processing_rate(QHBoxLayout* bar) {
    _processing_rate=new QLabel(this);_processing_rate->setObjectName("pipelineProcessingRate");
    _processing_rate->setTextFormat(Qt::PlainText);
    bar->addWidget(_processing_rate);
    connect(&_application,&GuiPresenter::processing_state_changed,this,&SchemeEditorWidget::refresh_processing_telemetry);
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_processing_telemetry);
    refresh_processing_telemetry();
}
void SchemeEditorWidget::setup_zoom_control(QHBoxLayout* bar) {
    auto *zoom=new QDoubleSpinBox(this); zoom->setObjectName("pipelineZoom");
    zoom->setMaximumWidth(72); zoom->setFixedHeight(24);
    zoom->setDecimals(0); zoom->setRange(20,300); zoom->setSuffix(" %"); zoom->setSingleStep(10); zoom->setValue(100);
    bar->addWidget(ui::make<QLabel>("Zoom",this)); bar->addWidget(zoom);
    connect(zoom,&QDoubleSpinBox::valueChanged,this,[this](double percent) {
        const auto center=_view->mapToScene(_view->viewport()->rect().center());
        static_cast<ProfiledGraphicsView *>(_view)->camera(percent/100,center);
    });
    connect(_view,&GraphicsView::scaleChanged,zoom,[zoom](double value) {
        const QSignalBlocker blocker(zoom); zoom->setValue(value*100);
    });
}
QPushButton* SchemeEditorWidget::setup_center_control(QHBoxLayout* bar) {
    auto *fit=ui::make<QPushButton>("Center",this); fit->setObjectName("fitPipeline");
    ui::compact_control(fit);
    ui::bind(fit,"toolTip","Fit the Data Flow in view (Alt+F)");
    bar->addWidget(fit);
    connect(fit,&QPushButton::clicked,this,[this] {
        fit_nodes();
    });
    return fit;
}
void SchemeEditorWidget::setup_splitter(QVBoxLayout* layout) {
    auto *splitter = new InspectorSplitter(Qt::Horizontal,this);
    _splitter=splitter; splitter->setObjectName("pipelineSplitter");
    setup_node_library(splitter);
    splitter->addWidget(_view);
    setup_inspector(splitter);
    splitter->setCollapsible(0,false); splitter->setCollapsible(1,false); splitter->setCollapsible(2,true);
    splitter->setStretchFactor(1,1); splitter->setSizes({170,750,280});
    ui::bind(splitter->handle(2),"toolTip","Drag to resize; click to collapse or expand parameters");
    splitter->toggle_right=[this] { set_inspector_expanded(_splitter->sizes().value(2)==0); };
    layout->addWidget(splitter, 1);
}
void SchemeEditorWidget::setup_node_library(QSplitter* splitter) {
    auto *library = new QTreeWidget(splitter); library->setObjectName("pipelineNodeLibrary");
    library->setBackgroundRole(QPalette::Window);
    library->viewport()->setBackgroundRole(QPalette::Window);
    library->viewport()->setAutoFillBackground(true);
    library->setHeaderHidden(true); library->setMinimumWidth(140);
    numeric_editor::populate_catalog(library);
    connect(&ui::Language::instance(),&ui::Language::changed,this,
            [this,library] {refresh_library_language(library);});
    library->setEnabled(!_unsupported_document);
    connect(library,&QTreeWidget::itemDoubleClicked,this,[this](QTreeWidgetItem *item) {
        const auto kind=item->data(0,Qt::UserRole).toString();
        if (!kind.isEmpty()) _scene->undoStack().push(new QtNodes::CreateCommand(_scene,kind,
            _view->mapToScene(_view->viewport()->rect().center())));
    });
}
void SchemeEditorWidget::refresh_library_language(QTreeWidget* library) {
    numeric_editor::populate_catalog(library);
    auto* title=findChild<QLabel *>("pipelineInspectorTitle");
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(_inspected_node);
    if(title && model)title->setText(model->caption());
}
void SchemeEditorWidget::setup_inspector(QSplitter* splitter) {
    auto *inspector = new QScrollArea(splitter); inspector->setObjectName("pipelineInspector");
    _inspector=inspector;
    inspector->setWidgetResizable(true);
    inspector->setFrameShape(QFrame::NoFrame);
    inspector->setBackgroundRole(QPalette::Window);
    inspector->viewport()->setBackgroundRole(QPalette::Window);
    inspector->setAutoFillBackground(true);
    auto *contents=new QWidget(inspector); _inspector_layout=new QVBoxLayout(contents);
    inspector->setWidget(contents); inspect_node(QtNodes::InvalidNodeId);
}
void SchemeEditorWidget::bind_scene_selection() {
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
    connect(_scene,&QGraphicsScene::selectionChanged,this,&SchemeEditorWidget::inspect_scene_selection);
}
void SchemeEditorWidget::inspect_scene_selection() {
    const auto selected=_scene->selectedItems();
    for(auto* item:selected)
        if(auto* wire=qgraphicsitem_cast<QtNodes::ConnectionGraphicsObject*>(item)) {
            inspect_connection(wire->connectionId());return;
        }
    if(selected.isEmpty())inspect_node(QtNodes::InvalidNodeId);
}
namespace {
QAction* preset_action(QMenu* menu,const char* title,const char* name) {
    auto* action=menu->addAction(ui::text(title));action->setObjectName(name);ui::bind(action,"text",title);return action;
}
}
QMenu* SchemeEditorWidget::create_preset_menu() {
    auto* presets=ui::tool_button(this,"Presets");presets->setObjectName("pipelinePresets");_presets_button=presets;presets->hide();
    auto* menu=new QMenu(presets);menu->setObjectName("pipelinePresetsMenu");ui::button_menu(presets,menu);ui::bind(menu,"title","Presets");return menu;
}
void SchemeEditorWidget::request_builtin(bool generator) {
    if(_application.graph_request_pending()) {set_status(ui::text("Wait for the pending graph command to finish before loading the active Data Flow."));return;}
    if(generator)emit generator_loopback_graph_requested();else emit default_graph_requested();
}
void SchemeEditorWidget::populate_user_presets(QMenu* user_presets,QMenu* delete_presets) {
    user_presets->clear();delete_presets->clear();
    const auto presets=_application.state().file_configs.ui.pipeline_presets;
    for(const auto& value:presets) {
        const auto preset=value.toObject();const auto id=preset.value("id").toString();
        if(id.isEmpty() || !preset.value("graph").isObject())continue;
        const auto name=preset.value("name").toString();auto* load=user_presets->addAction(name);
        connect(load,&QAction::triggered,this,[this,id]{load_preset(id);});auto* remove=delete_presets->addAction(name);
        connect(remove,&QAction::triggered,this,[this,id]{remove_preset(id);});
    }
    user_presets->setEnabled(!user_presets->isEmpty());delete_presets->setEnabled(!delete_presets->isEmpty());
}
void SchemeEditorWidget::setup_user_presets(QMenu* menu) {
    auto* users=menu->addMenu(ui::text("My presets"));ui::bind(users,"title","My presets");
    auto* save=preset_action(menu,"Save preset…","savePipelinePreset");
    auto* removals=menu->addMenu(ui::text("Delete preset"));ui::bind(removals,"title","Delete preset");
    auto refresh=[this,users,removals]{populate_user_presets(users,removals);};refresh();connect(menu,&QMenu::aboutToShow,this,refresh);
    connect(save,&QAction::triggered,this,[this]{
        bool accepted=false;const auto name=QInputDialog::getText(this,ui::text("Save preset"),ui::text("Preset name"),QLineEdit::Normal,QString{},&accepted);
        if(accepted && !name.trimmed().isEmpty())save_preset(name);
    });
}
void SchemeEditorWidget::setup_presets() {
    auto* menu=create_preset_menu();auto* reset=preset_action(menu,"Default","defaultPipeline");
    auto* loopback=preset_action(menu,"Generator and capture source","generatorLoopbackPreset");
    auto* active=preset_action(menu,"Applied Data Flow","loadActivePipeline");auto* generator=preset_action(menu,"Generator example","useTestSource");
    menu->addSeparator();setup_user_presets(menu);
    connect(reset,&QAction::triggered,this,[this]{request_builtin(false);});
    connect(loopback,&QAction::triggered,this,[this]{request_builtin(true);});connect(generator,&QAction::triggered,this,[this]{request_builtin(true);});
    connect(active,&QAction::triggered,this,&SchemeEditorWidget::load_active_pipeline);
}
void SchemeEditorWidget::bind_graph_model() {
    auto &model = *_application.state().data_flow_graph;
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model))numeric->set_undo_stack(&_scene->undoStack());
    const auto initial_nodes=model.allNodeIds();
    for(auto id:initial_nodes)wire_source_model(id);
    connect(&model,&DataFlowGraphModel::nodeCreated,this,&SchemeEditorWidget::wire_source_model);
    connect(&model,&DataFlowGraphModel::nodeCreated,this,&SchemeEditorWidget::update_graph_view_indicator);
    connect(&model,&DataFlowGraphModel::nodeUpdated,this,&SchemeEditorWidget::update_graph_view_indicator);
    refresh_graph_indicators();
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,&SchemeEditorWidget::refresh_graph_indicators);
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_graph_indicators);
    bind_graph_changes();bind_connection_diagnostics();
}
void SchemeEditorWidget::refresh_graph_indicators() {
    const auto nodes=_application.state().data_flow_graph->allNodeIds();
    for(auto id:nodes)update_graph_view_indicator(id);
}
void SchemeEditorWidget::bind_graph_changes() {
    auto& model=*_application.state().data_flow_graph;
    connect(_scene, &DataFlowGraphicsScene::modified, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodePositionUpdated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::nodeDeleted, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionCreated, this, &SchemeEditorWidget::changed);
    connect(&model, &DataFlowGraphModel::connectionDeleted, this, &SchemeEditorWidget::changed);
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model)){
        connect(numeric,&NumericEditorGraphModel::atomic_edit_finished,this,&SchemeEditorWidget::changed);
        connect(numeric,&NumericEditorGraphModel::immediate_apply_requested,this,[this]{
            if(_loading || !_live_apply_pending || _unsupported_document)return;
            _immediate_apply_pending=true;
            schedule_apply();
        });
    }
}
void SchemeEditorWidget::bind_connection_diagnostics() {
    auto& model=*_application.state().data_flow_graph;
    connect(&model,&DataFlowGraphModel::connectionCreated,this,&SchemeEditorWidget::refresh_connection_diagnostics);
    connect(&model,&DataFlowGraphModel::nodeUpdated,this,&SchemeEditorWidget::refresh_connection_diagnostics);
    connect(&ui::Language::instance(),&ui::Language::changed,this,&SchemeEditorWidget::refresh_connection_diagnostics);
    refresh_connection_diagnostics();
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&model))
        connect(numeric,&NumericEditorGraphModel::connection_diagnostics_changed,this,
                &SchemeEditorWidget::refresh_connection_diagnostics);
}
void SchemeEditorWidget::refresh_connection_diagnostics() {
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    if(!graph)return;
    const auto nodes=graph->allNodeIds();
    for(auto id:nodes)refresh_node_connection_diagnostics(graph,id);
}
void SchemeEditorWidget::refresh_node_connection_diagnostics(NumericEditorGraphModel* graph,QtNodes::NodeId id) {
    const auto edges=graph->allConnectionIds(id);
    for(auto edge:edges) {
        if(edge.outNodeId!=id)continue;
        auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);
        auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);
        auto* wire=_scene->connectionGraphicsObject(edge);
        if(from && to && wire){wire->setToolTip({});wire->update();}
    }
}
void SchemeEditorWidget::setup_editor_display(QHBoxLayout* bar) {
    auto* display=ui::tool_button(this,"Editor display");auto* displayMenu=new QMenu(display);display->setMenu(displayMenu);display->setPopupMode(QToolButton::InstantPopup);bar->addWidget(display);
    auto* displayForm=new QWidget(displayMenu);displayForm->setProperty("scrollSafeControls",true);auto* fields=new QFormLayout(displayForm);
    struct Setting{const char* key;const char* title;int value,minimum,maximum;};
    for(const auto setting:{Setting{"node_width","Block width",200,140,400},Setting{"port_spacing","Port spacing",76,56,160},Setting{"port_radius","Port radius",7,5,14},Setting{"port_font","Port font size",9,7,16}})
        add_editor_display_control(fields,displayForm,setting.key,setting.title,setting.value,setting.minimum,setting.maximum);
    auto* displayAction=new QWidgetAction(displayMenu);displayAction->setDefaultWidget(displayForm);displayMenu->addAction(displayAction);
}
void SchemeEditorWidget::add_editor_display_control(QFormLayout* fields,QWidget* parent,const char* key,const char* title,
    int fallback,int minimum,int maximum) {
    auto* control=new QSpinBox(parent);
    control->setRange(minimum,maximum);
    control->setValue(_application.state().file_configs.ui.editor_view.value(key).toInt(fallback));
    fields->addRow(ui::text(title),control);
    connect(control,&QSpinBox::valueChanged,this,[this,name=QString(key)](int value){set_editor_display_setting(name,value);});
}
void SchemeEditorWidget::set_editor_display_setting(const QString& key,int value) {
    _application.state().file_configs.ui.editor_view[key]=value;
    const auto nodes=_application.state().data_flow_graph->allNodeIds();
    for(auto id:nodes)if(auto* node=_scene->nodeGraphicsObject(id)) {
        node->setGeometryChanged();_scene->nodeGeometry().recomputeSize(id);
        node->updateQWidgetEmbedPos();node->moveConnections();node->update();
    }
    save_view();_application.state().editor_draft_modified=true;_save_timer.start();
}
void SchemeEditorWidget::initialize_document() {
    setObjectName("pipelineEditor");
    _document_template = _application.state().file_configs.scene_graph.isEmpty()?_application.state().data_flow_graph->save():_application.state().file_configs.scene_graph;
    editable_document(_document_template,_unsupported_document);
    retain_unrendered_connections();
    setStyle__();
    connect(&ui::Theme::instance(),&ui::Theme::changed,this,[this] {
        setStyle__();
        _view->setBackgroundBrush(QApplication::palette().color(QPalette::Window));
        _view->resetCachedContent();
        _scene->invalidate();_view->viewport()->update();
    });
}
void SchemeEditorWidget::configure_scene() {
    _scene->setNodeGeometry(std::make_unique<EditorNodeGeometry>(*_application.state().data_flow_graph,[this]{return _application.state().file_configs.ui.editor_view;}));
    _scene->setNodePainter(std::make_unique<EditorNodePainter>());
    _scene->setConnectionPainter(std::make_unique<EditorConnectionPainter>());
    _scene->setGroupingEnabled(false);
}
void SchemeEditorWidget::configure_view() {
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
}
void SchemeEditorWidget::initialize_preview_tracking() {
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
    connect(&_application,&GuiPresenter::active_graph_changed,this,&SchemeEditorWidget::observe_active_graph);
}
bool SchemeEditorWidget::active_adapters_match(const QJsonObject& graph) const {
    const auto nodes=graph.value("nodes").toArray();
    for(const auto value:nodes){
        const auto saved=value.toObject();
        auto* node=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(QtNodes::NodeId(saved.value("id").toInt()));
        if(!node || node->adapter()!=saved.value("adapter").toObject())return false;
    }
    return true;
}
void SchemeEditorWidget::reconcile_pending_apply(const QJsonObject& graph,const QJsonObject& processing) {
    if(!_live_apply_pending)return;
    if(processing_document(document())==processing && active_adapters_match(graph)) {
        _live_apply_pending=false;_immediate_apply_pending=false;_apply_timer.stop();
    } else schedule_apply(); // Include edits that arrived while Apply was pending.
}
void SchemeEditorWidget::observe_active_graph(const QJsonObject& graph) {
    const auto processing=processing_document(graph);const bool processing_changed=processing!=_active_processing_document;
    _active_processing_document=processing;record_active_source_adapters(graph);
    reconcile_pending_apply(graph,processing);
    if(processing_changed)clear_previews();sync_preview_subscription();
    bool sources_active=true;
    for(auto id:_application.state().data_flow_graph->allNodeIds())sources_active&=preview_source_active(id);
    if(processing_document(document())==_active_processing_document && sources_active)set_status({});
}
void SchemeEditorWidget::bind_preview_delivery() {
    connect(&_application,&GuiPresenter::numeric_frame_ready,this,&SchemeEditorWidget::on_numeric_preview);
    connect(&_application,&GuiPresenter::numeric_history_ready,this,&SchemeEditorWidget::on_history_preview);
    connect(&_application,&GuiPresenter::numeric_history_snapshot_ready,this,&SchemeEditorWidget::on_snapshot_preview);
    _preview_engine_state=int(_application.client().snapshot().statuses.value("analyzer").state);
    connect(&_application,&GuiPresenter::component_status_changed,this,[this](const QString& component,int state,const QString&){
        if(component!="analyzer" || state==_preview_engine_state)return;
        _preview_engine_state=state;sync_preview_subscription();
    });
}
void SchemeEditorWidget::on_numeric_preview(const pipeline::DeliveredNumericFrame& frame) {
    if(!accept_preview(frame,frame.status==signal_contracts::Status::ok))return;
    if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node))
        model->preview(frame);
}
void SchemeEditorWidget::on_history_preview(const pipeline::DeliveredNumericHistoryRow& row) {
    if(!accept_preview(row.compact,row.event==pipeline::NumericHistoryEvent::Row))return;
    if(row.compact.context && row.compact.context->contract) {
        const auto contract=row.compact.context->contract.view();
        const auto* numeric=std::get_if<signal_contracts::NumericContract>(&contract);
        if(numeric && numeric->semantic==signal_contracts::Semantic::History &&
           numeric->shape.rank==signal_contracts::Rank::Vector)
            if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(
                   row.compact.context->endpoint.node))model->preview(row);
    }
    _application.client().request_numeric_snapshots();
}
void SchemeEditorWidget::on_snapshot_preview(const pipeline::DeliveredNumericFrame& frame,
                                              quint64 ordinal,quint64 filled_count) {
    if(!accept_preview(frame,false))return;
    if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node))
        model->preview_history_snapshot(frame,ordinal,filled_count);
}
void SchemeEditorWidget::bind_application_feedback() {
    connect(&_application,&GuiPresenter::apply_status_changed,this,[this](const QString &status) {
        if(status.startsWith("Error: ")) set_status(ui::text("Not applied: ")+status.sliced(7));
        else if(status.startsWith(ui::text("Not applied: "))) set_status(status);
    });
    connect(&_application,&GuiPresenter::selected_device_changed,this,[this](const QJsonObject &draft,const QJsonObject &view) {
        _capture_state=int(pipeline::State::Stopped);_capture_error.clear();refresh_capture_status();
        load_document(draft,view,false,true);
    });
    connect(&_application,&GuiPresenter::views_cleared,this,&SchemeEditorWidget::clear_node_previews);
    connect(&_application,&GuiPresenter::graph_apply_finished,this,&SchemeEditorWidget::on_graph_apply_finished);
    connect(&_application, &GuiPresenter::saved_graph_warning_changed, this,
            [this](const QString &warning) { set_status(warning); });
}
void SchemeEditorWidget::clear_node_previews() {
    const auto nodes=_application.state().data_flow_graph->allNodeIds();
    for(const auto id:nodes)
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))model->clear_preview();
    _preview_times.clear();
}
void SchemeEditorWidget::on_graph_apply_finished(quint64 request,bool accepted,const QString& error) {
    if(accepted)refresh_capture_status();
    if(request!=_apply_request) {
        if(_immediate_apply_pending && _live_apply_pending)schedule_apply();
        return;
    }
    _apply_request=0;
    if(accepted && processing_document(document())!=processing_document(_application.client().snapshot().graph))
        schedule_apply();
    if(!accepted){_live_apply_pending=false;_immediate_apply_pending=false;_apply_timer.stop();}
    set_status(accepted ? QString{} : ui::text("Not applied: ")+error);
}
void SchemeEditorWidget::restore_initial_camera() {
    QString error;
    domain::NumericProcessingGraph draft;
    set_status(numeric_draft(draft,&error)?QString{}:ui::text("Draft not active: ")+error);
    const auto generation = _document_generation;
    QTimer::singleShot(0,this,[this,generation] {restore_saved_camera(generation);});
}
void SchemeEditorWidget::restore_saved_camera(quint64 generation) {
    if(generation!=_document_generation)return;
    _loading=true;
    const auto saved=_application.state().file_configs.ui.editor_view;
    if(_document_template.value("layout_pending").toBool()) {
        layout_nodes(false);
        _document_template.remove("layout_pending");
        _application.state().editor_draft_modified=true;
        _save_timer.start();
    }
    apply_saved_camera(saved);
    _loading=false;
}
void SchemeEditorWidget::apply_saved_camera(const QJsonObject& saved) {
    if(saved.value("version").toInt()==1) {
        const auto scale=saved.value("scale").toDouble(1);
        const auto x=saved.value("center_x").toDouble(),y=saved.value("center_y").toDouble();
        if(std::isfinite(scale) && std::isfinite(x) && std::isfinite(y))
            static_cast<ProfiledGraphicsView *>(_view)->camera(std::clamp(scale,0.2,3.0),{x,y});
        else static_cast<ProfiledGraphicsView *>(_view)->camera(1,node_bounds().center());
    } else fit_nodes();
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
static bool edit_execution_rate(QLineEdit* rate,NumericEditorGraphModel* graph) {
    const auto parts=rate->text().trimmed().split('/');
    bool ok=false,ok2=true;
    const auto numerator=parts.value(0).toULongLong(&ok);
    const auto denominator=parts.size()==2 ? parts[1].toULongLong(&ok2) : 1;
    if(!graph || parts.size()>2 || !ok || !ok2 || !numerator || !denominator ||
       denominator>std::uint64_t(INT64_MAX))return false;
    return graph->edit_execution_interval({std::int64_t(denominator),numerator});
}
QWidget* SchemeEditorWidget::execution_settings_widget(QWidget* parent) {
    auto* settings=new QWidget(parent);settings->setObjectName("pipelineExecutionSettings");
    auto* rate=new QLineEdit(settings);rate->setObjectName("pipelineAnalysisRate");rate->setMaximumWidth(96);
    ui::bind(rate,"toolTip","Analysis rate, Hz (integer or numerator/denominator)");
    auto* layout=new QVBoxLayout(settings);layout->setContentsMargins(0,0,0,0);
    layout->addWidget(ui::make<QLabel>("Analysis rate, Hz",settings));layout->addWidget(rate);
    auto* numericRate=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    auto refreshRate=[this,numericRate,rate]{refresh_execution_rate_text(numericRate,rate);};
    refreshRate();connect(numericRate,&NumericEditorGraphModel::atomic_edit_finished,rate,refreshRate);
    connect(this,&SchemeEditorWidget::execution_settings_changed,rate,refreshRate);
    connect(rate,&QLineEdit::editingFinished,this,[this,numericRate,rate,refreshRate]{
        if(!edit_execution_rate(rate,numericRate))set_status(ui::text("Invalid execution interval"));
        refreshRate();
    });
    return settings;
}
void SchemeEditorWidget::refresh_execution_rate_text(NumericEditorGraphModel* graph,QLineEdit* rate) {
    if(!graph)return;
    const auto interval=graph->numeric_graph().execution_interval;
    const QSignalBlocker blocker(rate);
    rate->setText(interval.numerator==1?QString::number(interval.denominator):
        QString::number(interval.denominator)+"/"+QString::number(interval.numerator));
}
void SchemeEditorWidget::refresh_processing_telemetry() {
    const auto snapshot=_application.client().snapshot();
    const auto telemetry=snapshot.diagnostics;
    const bool measured=refresh_processing_rate_label(telemetry);
    if(!isVisible())return;
    const bool current=_last_processing_document==_active_processing_document &&
        snapshot.statuses.value("analyzer").state==pipeline::State::Running;
    refresh_node_processing_activity(telemetry.value("node_stats").toArray(),current,measured);
}
bool SchemeEditorWidget::refresh_processing_rate_label(const QJsonObject& telemetry) {
    const double hz=telemetry.value("processing_hz").toDouble();
    const bool measured=telemetry.value("processing_measured").toBool() && std::isfinite(hz) && hz>=0;
    const auto text=ui::text("Processing: %1 Hz").arg(measured?QString::number(hz,'f',1):QStringLiteral("—"));
    if(_processing_rate && _processing_rate->text()!=text)_processing_rate->setText(text);
    const QMap<QString,const char*> bases{{"window_outputs","Window outputs"},{"mixer_outputs","Mixer outputs"},{"transform_outputs","Transform outputs"},{"source_outputs","Source outputs"}};
    const auto basis=bases.value(telemetry.value("processing_rate_basis").toString(),"Awaiting measurement");
    if(_processing_rate)_processing_rate->setToolTip(ui::text("Measured processing output rate; independent of the analysis target and display FPS.")+"\n"+
        ui::text("Node %1 · %2 · interval %3 s").arg(measured?telemetry.value("processing_rate_node").toVariant().toString():QStringLiteral("—"),ui::text(basis),QString::number(telemetry.value("measurement_seconds").toDouble(),'f',2)));
    return measured;
}
void SchemeEditorWidget::refresh_node_processing_activity(const QJsonArray& stats,bool current,bool measured) {
    QMap<int,QJsonObject> rows;
    for(const auto& value:stats){const auto row=value.toObject();rows.insert(row.value("node").toInt(-1),row);}
    const auto nodes=_application.state().data_flow_graph->allNodeIds();
    for(auto id:nodes)if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)) {
        const auto row=rows.value(int(id));
        model->set_processing_activity(current && measured && !row.isEmpty(),current && row.value("active").toBool(),row.value("calls_hz").toDouble());
    }
}
void SchemeEditorWidget::set_navigation_speed(float speed) {
    static_cast<ProfiledGraphicsView *>(_view)->set_navigation_speed(speed);
}
namespace {
class EditorDocumentSnapshot {
public:
    explicit EditorDocumentSnapshot(const QJsonObject& original) : _original(original) {
        const auto nodes=original.value("nodes").toArray();
        for (const auto value : nodes) {
            const auto node = value.toObject();
            _nodes[node.value("id").toInt()] = node;
        }
    }
    QJsonObject save(const QJsonObject& graph, const QJsonArray& unrendered) const {
        auto result = merge(_original, graph);
        result["nodes"] = nodes(graph);
        result["connections"] = connections(graph, unrendered);
        if (!result.contains("version")) result["version"] = 2;
        result.remove("layout_pending");
        return result;
    }
private:
    static QJsonObject merge(QJsonObject original, const QJsonObject& edited) {
        for (auto it = edited.begin(); it != edited.end(); ++it) original[it.key()] = it.value();
        return original;
    }
    QJsonArray nodes(const QJsonObject& graph) const {
        QJsonArray result;
        const auto original_nodes=_original.value("nodes").toArray();
        for (const auto value : original_nodes) {
            const auto node = value.toObject();
            if (!numeric_editor::catalog().contains(node.value("internal-data").toObject().value("model-name").toString()))
                result.append(node);
        }
        const auto edited_nodes=graph.value("nodes").toArray();
        for (const auto value : edited_nodes) {
            const auto edited = value.toObject();
            result.append(merge(_nodes.value(edited.value("id").toInt()), edited));
        }
        return result;
    }
    QJsonObject original_connection(const QJsonObject& edited) const {
        const auto original_connections=_original.value("connections").toArray();
        for (const auto value : original_connections) {
            const auto original = value.toObject();
            if (QtNodes::fromJson(original) == QtNodes::fromJson(edited)) return original;
        }
        return {};
    }
    QJsonArray connections(const QJsonObject& graph, const QJsonArray& unrendered) const {
        QJsonArray result;
        const auto connections=graph.value("connections").toArray();
        for (const auto value : connections) {
            const auto edited = value.toObject();
            result.append(merge(original_connection(edited), edited));
        }
        for (const auto value : unrendered) result.append(value);
        return result;
    }
    QJsonObject _original;
    QMap<int, QJsonObject> _nodes;
};
}

QJsonObject SchemeEditorWidget::document() const {
    return EditorDocumentSnapshot(_document_template).save(_application.state().data_flow_graph->save(), _unrendered_connections);
}
void SchemeEditorWidget::clear_previews() {
    _preview_times.clear();
    for(auto id:_application.state().data_flow_graph->allNodeIds())
        if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))model->clear_preview();
}
void SchemeEditorWidget::record_active_source_adapters(const QJsonObject& graph) {
    _active_source_adapters={};
    const auto nodes=graph.value("nodes").toArray();
    for(const auto& value:nodes){
        const auto node=value.toObject();const auto adapter=node.contains("adapter")?node.value("adapter").toObject():node.value("internal-data").toObject().value("adapter").toObject();
        _active_source_adapters[QString::number(node.value("id").toInteger())]=adapter;
    }
}
bool SchemeEditorWidget::preview_source_active(QtNodes::NodeId id) const {
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(!model || model->name()!="AudioSource")return true;
    return _active_source_adapters.value(QString::number(id)).toObject().value("device_identity").toString()==model->adapter().value("device_identity").toString();
}
static const char* preview_activity_key(pipeline::State state,bool draft_active) {
    if(state==pipeline::State::Stopped)return "Processing stopped";
    if(!draft_active)return "Draft not active";
    if(state==pipeline::State::Starting || state==pipeline::State::Recovering)return "Processing starting";
    if(state==pipeline::State::Failed)return "Processing failed";
    return nullptr;
}
void SchemeEditorWidget::refresh_preview_activity() {
    const auto state=_application.client().snapshot().statuses.value("analyzer").state;
    const auto* key=preview_activity_key(state,_last_processing_document==_active_processing_document);
    const QString activity=key?QString::fromLatin1(key):QString{};
    for(auto id:_application.state().data_flow_graph->allNodeIds())if(auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id))model->set_preview_activity(activity.isEmpty() && !preview_source_active(id)?QStringLiteral("Draft not active"):activity);
}
void SchemeEditorWidget::sync_preview_subscription() {
    refresh_preview_activity();
    std::vector<pipeline::EndpointIdentity> endpoints;
    domain::NumericProcessingGraph draft;
    if(isVisible() && _last_processing_document==_active_processing_document && numeric_draft(draft))
        for(const auto& node:draft.nodes)if(_preview_nodes.contains(QtNodes::NodeId(node.id)) && preview_source_active(QtNodes::NodeId(node.id))) {
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
void SchemeEditorWidget::restore_node_positions(const QJsonObject& positions) {
    if(positions.isEmpty())return;
    // A load can already have queued a default layout for the first Show event.
    // Restoring an explicit layout cancels it, including its deferred fit/zoom.
    _initial_layout_pending=false;_initial_layout_fit=false;
    QScopedValueRollback<bool> loading(_loading,true);
    auto& graph=*_application.state().data_flow_graph;
    for(auto it=positions.begin();it!=positions.end();++it){
        bool valid=false;const auto id=it.key().toUInt(&valid);const auto point=it.value().toObject();
        if(valid && graph.nodeExists(id))graph.setNodeData(id,QtNodes::NodeRole::Position,QPointF(point.value("x").toDouble(),point.value("y").toDouble()));
    }
}
void SchemeEditorWidget::schedule_initial_layout() {
    if(!_initial_layout_pending || _initial_layout_scheduled)return;
    _initial_layout_scheduled=true;const auto generation=_document_generation;
    QTimer::singleShot(0,this,[this,generation]{apply_initial_layout(generation);});
}
void SchemeEditorWidget::apply_initial_layout(quint64 generation) {
    _initial_layout_scheduled=false;
    if(generation!=_document_generation){schedule_initial_layout();return;}
    if(!_initial_layout_pending || !isVisible() || _view->viewport()->width()<2 ||
       _view->viewport()->height()<2)return;
    _initial_layout_pending=false;_loading=true;
    layout_nodes(false);if(_initial_layout_fit)fit_nodes();_loading=false;
    save_view();_application.state().editor_draft_modified=true;_save_timer.start();
}
bool SchemeEditorWidget::preview_matches_active(const pipeline::DeliveredNumericFrame& frame) const {
    if(!isVisible() || !frame.context || _last_processing_document!=_active_processing_document)return false;
    if(!_preview_nodes.contains(QtNodes::NodeId(frame.context->endpoint.node)))return false;
    const auto snapshot=_application.client().snapshot();
    if(snapshot.statuses.value("analyzer").state!=pipeline::State::Running)return false;
    return frame.delivery.epoch==snapshot.epoch && frame.delivery.graph_revision==snapshot.graph_revision &&
           frame.context->endpoint.graph_id==_active_processing_document.value("graph_id").toString().toStdString();
}
bool SchemeEditorWidget::preview_port_available(const pipeline::DeliveredNumericFrame& frame) const {
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(frame.context->endpoint.node);
    if(!model || !model->numeric_node() || !preview_source_active(QtNodes::NodeId(frame.context->endpoint.node)))return false;
    const auto& node=*model->numeric_node();const auto& endpoint=frame.context->endpoint;
    const auto& ports=endpoint.direction==domain::PortDirection::Output?node.outputs:node.inputs;
    return std::any_of(ports.begin(),ports.end(),[&](const auto& port){return port.id==endpoint.port;});
}
bool SchemeEditorWidget::accept_preview(const pipeline::DeliveredNumericFrame& frame,bool throttle) {
    if(!preview_matches_active(frame) || !preview_port_available(frame))return false;
    const auto& endpoint=frame.context->endpoint;
    const auto now=_preview_clock.elapsed();const auto found=_preview_times.find(endpoint);
    if(throttle && found!=_preview_times.end() && now-found->second<100)return false;
    _preview_times[endpoint]=now;return true;
}
bool SchemeEditorWidget::numeric_draft(domain::NumericProcessingGraph& out,QString* error,bool resolve_native) const {
    auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    if(!numeric){if(error)*error=ui::text("Numeric editor unavailable");return false;}
    if(_unsupported_document){if(error)*error=ui::text("Draft contains unsupported nodes");return false;}
    return numeric->export_numeric_graph(out,error,resolve_native);
}
QJsonObject SchemeEditorWidget::graph_adapter_metadata() const {
    auto& graph=*_application.state().data_flow_graph;auto ids=graph.allNodeIds();std::vector<QtNodes::NodeId> sorted(ids.begin(),ids.end());std::sort(sorted.begin(),sorted.end());QJsonArray nodes;
    for(auto id:sorted)if(auto* model=graph.delegateModel<AudioNodeModel>(id))nodes.append(QJsonObject{{"id",qint64(id)},{"adapter",model->adapter()}});
    return {{"graph_id",graph.save().value("graph_id")},{"nodes",nodes}};
}
static bool decode_graph_adapter_value(const QJsonValue& value,QJsonObject& adapter,QString* error) {
    const auto result=desktop::decode_numeric_node_adapter(
        QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact),adapter);
    if(result)return true;
    if(error)*error=result.error;
    return false;
}
bool SchemeEditorWidget::decode_graph_adapters(const QJsonObject& node_adapters,
                                                AdapterMap& checked,QString* error) const {
    auto& graph=*_application.state().data_flow_graph;
    for(auto it=node_adapters.begin();it!=node_adapters.end();++it){bool valid=false;const auto id=it.key().toUInt(&valid);QJsonObject adapter;
        if(!valid || QString::number(id)!=it.key() || !graph.delegateModel<AudioNodeModel>(id) || !it.value().isObject()){if(error)*error=ui::text("Invalid network source settings");return false;}
        if(!decode_graph_adapter_value(it.value(),adapter,error))return false;
        checked[id]=std::move(adapter);
    }
    return true;
}
bool SchemeEditorWidget::apply_graph_adapters(const AdapterMap& checked,QString* error) {
    auto& graph=*_application.state().data_flow_graph;
    AdapterMap previous;
    for(auto it=checked.begin();it!=checked.end();++it)previous[it.key()]=graph.delegateModel<AudioNodeModel>(it.key())->adapter();
    const auto prior=_loading;_loading=true;
    for(auto it=checked.begin();it!=checked.end();++it)graph.delegateModel<AudioNodeModel>(it.key())->set_adapter(it.value());
    if(auto* numeric=dynamic_cast<NumericEditorGraphModel*>(&graph);numeric && !numeric->synchronize_native_sources(error)) {
        restore_previous_adapters(previous);
        _loading=prior;return false;
    }
    _loading=prior;
    return true;
}
void SchemeEditorWidget::restore_previous_adapters(const AdapterMap& previous) {
    auto& graph=*_application.state().data_flow_graph;
    for(auto it=previous.begin();it!=previous.end();++it)
        graph.delegateModel<AudioNodeModel>(it.key())->set_adapter(it.value());
}
bool SchemeEditorWidget::restore_graph_adapters(const QJsonObject& node_adapters,QString* error) {
    AdapterMap checked;
    if(!decode_graph_adapters(node_adapters,checked,error) || !apply_graph_adapters(checked,error))return false;
    auto& graph=*_application.state().data_flow_graph;
    _last_processing_document=processing_document(graph.save());
    _last_adapter_metadata=graph_adapter_metadata();sync_preview_subscription();return true;
}
bool SchemeEditorWidget::load_numeric_draft(const domain::NumericProcessingGraph& graph,QString* error,const QJsonObject& node_adapters) {
    auto* numeric=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());if(!numeric){if(error)*error=ui::text("Numeric editor unavailable");return false;}
    domain::NumericProcessingGraph prepared;
    if(!prepare_draft_import(numeric,graph,node_adapters,prepared,error))return false;
    if(!commit_draft_import(numeric,prepared,node_adapters,error))return false;
    finish_draft_import(numeric);
    return true;
}
bool SchemeEditorWidget::prepare_draft_import(NumericEditorGraphModel* numeric,
                                               const domain::NumericProcessingGraph& graph,
                                               const QJsonObject& node_adapters,
                                               domain::NumericProcessingGraph& prepared,QString* error) {
    if(!numeric->prepare_numeric_import(graph,node_adapters,prepared,error))return false;
    NumericEditorGraphModel candidate(numeric->dataModelRegistry());candidate.set_admission_limits(numeric->admission_limits());
    return candidate.set_numeric_graph(prepared,error,node_adapters);
}
bool SchemeEditorWidget::commit_draft_import(NumericEditorGraphModel* numeric,
                                              const domain::NumericProcessingGraph& prepared,
                                              const QJsonObject& node_adapters,QString* error) {
    // Complete the model transaction before altering inspector, pending work or
    // Undo. A native discovery failure must leave the entire editor untouched.
    const auto prior_loading=_loading;_loading=true;
    QSignalBlocker graph_blocker(numeric),scene_blocker(_scene);
    if(!numeric->set_numeric_graph(prepared,error,node_adapters)){_loading=prior_loading;return false;}
    ++_document_generation;_initial_layout_pending=false;_live_apply_pending=false;_immediate_apply_pending=false;
    _apply_timer.stop();_save_timer.stop();_preview_times.clear();inspect_node(QtNodes::InvalidNodeId);
    _scene->onModelReset();_scene->undoStack().clear();
    return true;
}
void SchemeEditorWidget::finish_draft_import(NumericEditorGraphModel* numeric) {
    _document_template=numeric->save();_unrendered_connections={};_unsupported_document=false;
    _view->setInteractive(true);if(auto* library=findChild<QTreeWidget*>("pipelineNodeLibrary"))library->setEnabled(true);
    const auto nodes=numeric->allNodeIds();
    for(auto id:nodes)wire_source_model(id);
    _last_processing_document=processing_document(numeric->save());
    _last_adapter_metadata=graph_adapter_metadata();
    _loading=false;_application.state().editor_draft_modified=false;set_status({});
    emit execution_settings_changed();
    sync_preview_subscription();
}
static bool consume_rendered_connection(QJsonArray& rendered,QtNodes::ConnectionId id) {
    for(int index=0;index<rendered.size();++index)
        if(QtNodes::fromJson(rendered[index].toObject())==id){rendered.removeAt(index);return true;}
    return false;
}
void SchemeEditorWidget::retain_unrendered_connections() {
    _unrendered_connections={};
    auto rendered=_application.state().data_flow_graph->save().value("connections").toArray();
    const auto saved_connections=_document_template.value("connections").toArray();
    for(const auto &value:saved_connections) {
        const auto id=QtNodes::fromJson(value.toObject());
        if(!consume_rendered_connection(rendered,id))_unrendered_connections.append(value);
    }
    _unsupported_document |= !_unrendered_connections.isEmpty();
}
void SchemeEditorWidget::clear_inspector() {
    while(auto* item=_inspector_layout->takeAt(0)){delete item->widget();delete item;}
}
void SchemeEditorWidget::append_inspector_title(AudioNodeModel* model) {
    auto* title=new QLabel(model->caption(),this);title->setWordWrap(true);title->setTextFormat(Qt::PlainText);title->setObjectName("pipelineInspectorTitle");
    title->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);title->setMinimumWidth(0);_inspector_layout->addWidget(title);
    const QPointer<AudioNodeModel> guard(model);
    connect(model,&AudioNodeModel::presentation_changed,title,[guard,title]{if(guard)title->setText(guard->caption());});
}
static QString input_summary_text(const AudioNodeModel* model) {
    QStringList descriptions;
    if(!model || !model->numeric_node())return {};
    for(const auto& port:model->numeric_node()->inputs)
        descriptions.append(ui::text("Input port %1").arg(port.id)+" · "+numeric_editor::port_caption(port));
    return descriptions.join('\n');
}
void SchemeEditorWidget::append_input_summary(AudioNodeModel* model) {
    auto* inputs=new QLabel(this);inputs->setObjectName("pipelineInputSummary");inputs->setTextFormat(Qt::PlainText);inputs->setWordWrap(true);
    const QPointer<AudioNodeModel> guard(model);
    auto refresh=[guard,inputs]{
        const auto description=input_summary_text(guard.data());
        inputs->setText(description);inputs->setVisible(!description.isEmpty());
    };
    refresh();connect(&ui::Language::instance(),&ui::Language::changed,inputs,refresh);connect(model,&AudioNodeModel::parameter_structure_changed,inputs,refresh);
    _inspector_layout->addWidget(inputs);
}
void SchemeEditorWidget::rename_node(QtNodes::NodeId id) {
    const QPointer<AudioNodeModel> model(_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id));if(!model)return;
    bool accepted=false;const auto name=QInputDialog::getText(this,ui::text("Rename block"),ui::text("Block name (empty restores default)"),QLineEdit::Normal,model->presentation().value("display_name").toString(),&accepted);
    if(accepted && model)model->edit_display_name(name);
}
void SchemeEditorWidget::append_rename_action(AudioNodeModel* model,QtNodes::NodeId id) {
    const bool graph=numeric_editor::is_graph_view(model->name());
    auto* rename=ui::make<QPushButton>(graph?"Rename graph":"Rename block",this);rename->setObjectName(graph?"pipelineRenameGraph":"pipelineRenameNode");
    rename->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);_inspector_layout->addWidget(rename);
    connect(rename,&QPushButton::clicked,this,[this,id,graph]{if(graph)emit graph_rename_requested(int(id));else rename_node(id);});
}
void SchemeEditorWidget::refresh_node_timing(QtNodes::NodeId id,QLabel* timing) {
    const auto telemetry=_application.client().snapshot().diagnostics;QJsonObject row;
    const auto stats=telemetry.value("node_stats").toArray();
    for(const auto& value:stats)if(value.toObject().value("node").toInt(-1)==int(id)){row=value.toObject();break;}
    timing->setVisible(!row.isEmpty());if(row.isEmpty())return;
    const auto calls=row.value("calls").toString().toULongLong(),total=row.value("self_nanoseconds").toString().toULongLong();
    timing->setText(ui::text("Calls: %1 · errors: %2").arg(QString::number(calls),row.value("errors").toString())+"\n"+
        ui::text("Self time, average/max: %1 / %2 ms").arg(QString::number(calls?double(total)/double(calls)/1e6:0,'f',3),QString::number(row.value("max_self_nanoseconds").toString().toDouble()/1e6,'f',3))+"\n"+
        ui::text("Deadline misses: %1").arg(row.value("deadline_misses").toString()));
}
void SchemeEditorWidget::append_node_diagnostics(QtNodes::NodeId id) {
    auto* diagnostics=new QWidget(this);diagnostics->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    auto* layout=new QVBoxLayout(diagnostics);layout->setContentsMargins(0,0,0,0);
    auto* heading=ui::make<QLabel>("Diagnostics / display",diagnostics);heading->setWordWrap(true);layout->addWidget(heading);
    auto* timing=new QLabel(diagnostics);timing->setObjectName("pipelineNodeTiming");timing->setWordWrap(true);timing->setTextFormat(Qt::PlainText);layout->addWidget(timing);
    auto refresh=[this,id,timing]{refresh_node_timing(id,timing);};refresh();
    connect(&_application,&GuiPresenter::processing_state_changed,timing,refresh);connect(&ui::Language::instance(),&ui::Language::changed,timing,refresh);
    _inspector_layout->addWidget(diagnostics);
}
void SchemeEditorWidget::inspect_node(QtNodes::NodeId id) {
    if(!_inspector_layout)return;
    _inspected_node=id;_preview_nodes.clear();clear_inspector();
    auto* model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(model)populate_node_inspector(model,id);
    else populate_empty_inspector();
    _inspector_layout->addStretch();sync_preview_subscription();
}
void SchemeEditorWidget::populate_node_inspector(AudioNodeModel* model,QtNodes::NodeId id) {
    _preview_nodes.insert(id);model->clear_preview();
    append_inspector_title(model);append_input_summary(model);
    _inspector_layout->addWidget(model->preview_widget(this));
    _inspector_layout->addWidget(model->parameter_widget(this));
    append_rename_action(model,id);append_node_diagnostics(id);
}
void SchemeEditorWidget::populate_empty_inspector() {
    auto* placeholder=ui::make<QLabel>("Select a node to see parameters",_inspector->widget());
    placeholder->setObjectName("pipelineInspectorPlaceholder");placeholder->setWordWrap(true);
    _inspector_layout->addWidget(placeholder);
}
namespace {
class ConnectionInspector final:public QWidget {
    QPointer<NumericEditorGraphModel> graph;
    QtNodes::ConnectionId edge;
    QLabel* contracts=nullptr;QLabel* risk=nullptr;QLabel* effects=nullptr;QLabel* failure=nullptr;
    QComboBox* policy=nullptr;
    static QLabel* label(QVBoxLayout* layout,const char* name={}) {
        auto* value=new QLabel(layout->parentWidget());value->setWordWrap(true);value->setTextFormat(Qt::PlainText);
        value->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);if(name)value->setObjectName(name);layout->addWidget(value);return value;
    }
    void setup(){
        setProperty("scrollSafeControls",true);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
        auto* title=label(layout,"pipelineInspectorTitle");ui::bind(title,"text","Connection conversion");
        contracts=label(layout);risk=label(layout,"connectionRisk");policy=new QComboBox(this);policy->setObjectName("connectionConversionPolicy");layout->addWidget(policy);
        effects=label(layout);failure=label(layout,"connectionRuntimeError");
    }
    void refresh_policy(){
        const QSignalBlocker block(policy);policy->clear();
        for(std::uint8_t mode=0;mode<4;++mode)policy->addItem(numeric_editor::conversion_policy_caption(mode));
        policy->setCurrentIndex(graph->connection_policy(edge));
    }
    QString endpoints(AudioNodeModel* from,AudioNodeModel* to)const {
        return from->caption()+" #"+QString::number(edge.outNodeId)+" → "+to->caption()+" #"+QString::number(edge.inNodeId);
    }
    void show_configuration_contracts(AudioNodeModel* from,AudioNodeModel* to) {
        contracts->setText(endpoints(from,to)+"\n\n"+from->portCaption(QtNodes::PortType::Out,edge.outPortIndex)+
                           "\n↓\n"+to->portCaption(QtNodes::PortType::In,edge.inPortIndex));
        policy->setEnabled(false);effects->clear();failure->clear();
    }
    bool show_data_contracts(AudioNodeModel* from,AudioNodeModel* to) {
        const auto& outputs=from->numeric_node()->outputs;
        const auto& inputs=to->numeric_node()->inputs;
        if(edge.outPortIndex>=outputs.size() || edge.inPortIndex>=inputs.size())return false;
        const auto& output=outputs[edge.outPortIndex];const auto& input=inputs[edge.inPortIndex];
        contracts->setText(endpoints(from,to)+"\n\n"+numeric_editor::contract_details(output)+
                           "\n↓\n"+numeric_editor::contract_details(input));
        policy->setEnabled(!domain::processing_ports_compatible(output,input));
        return true;
    }
    bool refresh_contracts(){
        auto* from=graph->delegateModel<AudioNodeModel>(edge.outNodeId);auto* to=graph->delegateModel<AudioNodeModel>(edge.inNodeId);
        if(!from || !to || !from->numeric_node() || !to->numeric_node())return false;
        if(graph->configuration_binding(edge)){
            show_configuration_contracts(from,to);return false;
        }
        return show_data_contracts(from,to);
    }
    void refresh(){
        if(!graph || !graph->connectionExists(edge))return;
        refresh_policy();risk->setText(graph->connection_status(edge));if(!refresh_contracts())return;
        effects->setText(numeric_editor::conversion_policy_details(graph->connection_policy(edge))+"\n\n"+graph->connection_effects(edge));
        const auto actual=graph->connection_failure(edge);failure->setText(actual.isEmpty()?ui::text("No observed runtime conversion error"):ui::text("Runtime error: %1").arg(actual));
    }
    void bind_diagnostics(std::function<void()> invalidate){
        for(auto id:{edge.outNodeId,edge.inNodeId})if(auto* node=graph->delegateModel<AudioNodeModel>(id))
            connect(node,&AudioNodeModel::diagnostics_changed,this,[this,invalidate]{refresh();invalidate();});
    }
public:
    ConnectionInspector(NumericEditorGraphModel* model,QtNodes::ConnectionId connection,QWidget* parent,std::function<void()> dismissed,std::function<void()> invalidate):QWidget(parent),graph(model),edge(connection){
        setup();connect(policy,&QComboBox::currentIndexChanged,this,[this](int mode){if(graph && mode>=0)graph->edit_connection_policy(edge,std::uint8_t(mode));});
        connect(model,&DataFlowGraphModel::nodeUpdated,this,[this]{refresh();});connect(&ui::Language::instance(),&ui::Language::changed,this,[this]{refresh();});
        bind_diagnostics(std::move(invalidate));
        connect(model,&DataFlowGraphModel::connectionDeleted,this,[connection,dismissed](QtNodes::ConnectionId removed){if(removed==connection)dismissed();});refresh();
    }
};
}
void SchemeEditorWidget::inspect_connection(QtNodes::ConnectionId edge) {
    auto* graph=dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    if(!graph || !graph->connectionExists(edge) || !_inspector_layout)return;
    _inspected_node=QtNodes::InvalidNodeId;_preview_nodes={edge.outNodeId,edge.inNodeId};clear_inspector();
    auto* panel=new ConnectionInspector(graph,edge,_inspector->widget(),[this]{inspect_node(QtNodes::InvalidNodeId);},[this]{_scene->invalidate();});
    _inspector_layout->addWidget(panel);_inspector_layout->addStretch();set_inspector_expanded(true);sync_preview_subscription();
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
static GraphViewIndicator* graph_indicator(QtNodes::NodeGraphicsObject* item) {
    const auto children=item->childItems();
    for(auto* child:children)
        if(child->type()==GraphViewIndicator::Type)return static_cast<GraphViewIndicator*>(child);
    return nullptr;
}
void SchemeEditorWidget::update_graph_view_indicator(QtNodes::NodeId id) {
    auto *item=_scene->nodeGraphicsObject(id);
    auto *model=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id);
    if(!item || !model) return;
    auto* indicator=graph_indicator(item);
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
    if(!add_preset_presentation(preset))return false;
    presets.append(preset);
    if (_application.state().file_configs.ui.autosave_enabled) emit _application.on_config_updated();
    return true;
}
bool SchemeEditorWidget::add_preset_presentation(QJsonObject& preset) const {
    if(!_preset_presentation_provider)return true;
    const auto presentation=_preset_presentation_provider();
    if(presentation.isEmpty())return true;
    QJsonObject checked;
    if(!desktop::decode_numeric_preset_presentation(
           QJsonDocument(presentation).toJson(QJsonDocument::Compact),checked))return false;
    preset["presentation"]=checked;
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
namespace {
class EditorDocumentImport {
public:
    explicit EditorDocumentImport(const QJsonObject& source) : _source(source), _nodes(source.value("nodes").toArray()) {}
    bool prepare(NumericEditorGraphModel& model, const QJsonObject& current, QJsonObject& visual, QString* error) {
        domain::NumericProcessingGraph incoming, prepared;
        if (!owned_processing_document(_source, incoming, error) || !read_adapters(error)) return false;
        if (!model.prepare_numeric_import(incoming, _adapters, prepared, error)) return false;
        NumericEditorGraphModel candidate(model.dataModelRegistry());
        candidate.set_admission_limits(model.admission_limits());
        if (!candidate.set_numeric_graph(prepared, error, _adapters)) return false;
        visual = is_typed() ? typed_visual(candidate, current) : prepared_visual(candidate);
        return true;
    }
private:
    bool is_typed() const { return !_nodes.isEmpty() && _nodes.first().toObject().contains("kind"); }
    bool read_adapters(QString* error) {
        for (const auto value : std::as_const(_nodes)) {
            const auto node = value.toObject();
            const auto adapter = node.contains("kind") ? node.value("adapter") : node.value("internal-data").toObject().value("adapter");
            if (adapter.isUndefined()) continue;
            QJsonObject checked;
            if (!adapter.isObject() || !desktop::decode_numeric_node_adapter(QJsonDocument(adapter.toObject()).toJson(QJsonDocument::Compact), checked)) {
                if (error) *error = ui::text("Invalid network source settings");
                return false;
            }
            _adapters[QString::number(node.value("id").toInteger())] = checked;
        }
        return true;
    }
    QJsonObject prepared_visual(NumericEditorGraphModel& candidate) const {
        auto visual = _source;
        QJsonArray nodes;
        for (const auto value : std::as_const(_nodes)) {
            auto node = value.toObject();
            auto data = node.value("internal-data").toObject();
            const auto typed = candidate.saveNode(QtNodes::NodeId(node.value("id").toInteger())).value("internal-data").toObject();
            data["numeric_processing"] = typed.value("numeric_processing");
            node["internal-data"] = data;
            nodes.append(node);
        }
        visual["nodes"] = nodes;
        return visual;
    }
    static QMap<int, QJsonObject> positions(const QJsonObject& document) {
        QMap<int, QJsonObject> result;
        const auto nodes = document.value("nodes").toArray();
        for (const auto value : nodes) {
            const auto node = value.toObject();
            result[node.value("id").toInt()] = node.value("position").toObject();
        }
        return result;
    }
    void collect_typed_metadata(QMap<int,QJsonObject>& positions, QMap<int,QJsonObject>& presentations, QMap<int,QJsonObject>& adapters) const {
        for (const auto value : std::as_const(_nodes)) {
            const auto node = value.toObject();
            const auto id = node.value("id").toInt();
            presentations[id] = node.value("presentation").toObject();
            adapters[id] = node.value("adapter").toObject();
            if (node.value("position").isObject()) positions[id] = node.value("position").toObject();
        }
    }
    static QJsonObject project_typed_node(QJsonObject node, const QMap<int,QJsonObject>& positions, const QMap<int,QJsonObject>& presentations, const QMap<int,QJsonObject>& adapters) {
        const auto id = node.value("id").toInt();
        const auto position = positions.value(id);
        if (!position.isEmpty()) node["position"] = position;
        auto data = node.value("internal-data").toObject();
        data["presentation"] = presentations.value(id);
        data["adapter"] = adapters.value(id);
        node["internal-data"] = data;
        return node;
    }
    QJsonObject typed_node_extensions(QJsonObject visual) const {
        QJsonObject original;
        for (const auto value : std::as_const(_nodes)) {
            const auto node = value.toObject();
            if (node.value("id") == visual.value("id")) { original = node; break; }
        }
        // Canonical processing fields are represented by numeric_processing;
        // only source extensions become siblings in the visual node.
        for (const auto* key : {"id", "kind", "enabled", "parameters", "inputs", "outputs", "position", "presentation", "adapter"})
            original.remove(key);
        for (auto it = visual.begin(); it != visual.end(); ++it) original[it.key()] = it.value();
        return original;
    }

    QJsonObject typed_visual_base(const QJsonObject& prepared) const {
        auto visual = _source;
        visual.remove("configuration_bindings"); // Represented by visual configuration edges.
        visual.remove("layout_pending"); // Explicit positions take precedence over a source marker.
        for (auto it = prepared.begin(); it != prepared.end(); ++it) visual[it.key()] = it.value();
        return visual;
    }
    QJsonObject typed_visual(NumericEditorGraphModel& candidate, const QJsonObject& current) const {
        const auto prepared = candidate.save();
        auto visual=typed_visual_base(prepared);
        auto saved_positions = positions(current);
        QMap<int,QJsonObject> presentations, adapters;
        collect_typed_metadata(saved_positions, presentations, adapters);
        QJsonArray nodes;
        const auto prepared_nodes = prepared.value("nodes").toArray();
        for (const auto value : prepared_nodes)
            nodes.append(typed_node_extensions(project_typed_node(value.toObject(), saved_positions, presentations, adapters)));
        visual["nodes"] = nodes;
        if (saved_positions.isEmpty()) visual["layout_pending"] = true;
        return visual;
    }
    QJsonObject _source;
    QJsonArray _nodes;
    QJsonObject _adapters;
};
}

bool SchemeEditorWidget::prepare_document_visual(const QJsonObject& source, bool active, QJsonObject& visual) {
    auto* model = dynamic_cast<NumericEditorGraphModel*>(_application.state().data_flow_graph.get());
    QString error;
    EditorDocumentImport import(source);
    if (model && import.prepare(*model, active ? document() : QJsonObject{}, visual, &error)) return true;
    set_status(ui::text("Not applied: ") + error);
    return false;
}

void SchemeEditorWidget::begin_document_load(const QJsonObject& visual, bool switch_device) {
    if (!switch_device) flush_presentation();
    ++_document_generation;
    _live_apply_pending = false;
    _immediate_apply_pending = false;
    _initial_layout_pending = false;
    _document_template = visual;
    inspect_node(QtNodes::InvalidNodeId);
    _apply_request = 0;
    _save_timer.stop();
    _apply_timer.stop();
    _loading = true;
}

void SchemeEditorWidget::rebuild_document_graph() {
    auto& graph = *_application.state().data_flow_graph;
    {
        const QSignalBlocker graph_blocker(&graph), scene_blocker(_scene);
        const auto ids = graph.allNodeIds();
        for (const auto id : ids) graph.deleteNode(id);
        graph.load(editable_document(_document_template, _unsupported_document));
        retain_unrendered_connections();
        _scene->onModelReset();
        _scene->undoStack().clear();
    }
    // nodeCreated is blocked during replacement; bind all new delegates now.
    for (const auto id : graph.allNodeIds()) wire_source_model(id);
    for (const auto id : graph.allNodeIds()) update_graph_view_indicator(id);
}

void SchemeEditorWidget::restore_document_view(const QJsonObject& view) {
    if (_document_template.value("layout_pending").toBool() && !_unsupported_document) {
        _document_template.remove("layout_pending");
        _initial_layout_pending = true;
        _initial_layout_fit = view.isEmpty();
    }
    if (view.isEmpty()) return;
    const double scale = view.value("scale").toDouble(1);
    const double x = view.value("center_x").toDouble(), y = view.value("center_y").toDouble();
    if (std::isfinite(scale) && std::isfinite(x) && std::isfinite(y))
        static_cast<ProfiledGraphicsView*>(_view)->camera(std::clamp(scale, 0.2, 3.0), {x, y});
}

void SchemeEditorWidget::finish_document_load(bool active, bool switch_device) {
    _view->setInteractive(!_unsupported_document);
    sync_preferences();
    if (auto* library = findChild<QTreeWidget*>("pipelineNodeLibrary")) library->setEnabled(!_unsupported_document);
    save_view();
    _loading = false;
    clear_previews();
    record_loaded_document(active,switch_device);
    sync_preview_subscription();
    if (_unsupported_document) set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
    schedule_initial_layout();
    emit execution_settings_changed();
}
void SchemeEditorWidget::record_loaded_document(bool active,bool switch_device) {
    if (active || switch_device) {
        _last_adapter_metadata = graph_adapter_metadata();
        _application.state().editor_draft_modified = false;
        _last_processing_document = processing_document(_application.state().data_flow_graph->save());
    } else changed();
    if (switch_device && !_unsupported_document) { _live_apply_pending = true; schedule_apply(); }
}

bool SchemeEditorWidget::load_document(const QJsonObject& source, const QJsonObject& view, bool active, bool switch_device) {
    QJsonObject visual;
    if (!prepare_document_visual(source, active, visual)) return false;
    begin_document_load(visual, switch_device);
    rebuild_document_graph();
    restore_document_view(view);
    finish_document_load(active, switch_device);
    return true;
}
static bool valid_preset_graph(const QJsonObject& document) {
    if(!document.value("nodes").isArray() || !document.value("connections").isArray())return false;
    QSet<int> ids;
    const auto nodes=document.value("nodes").toArray();
    for(const auto& value:nodes) {
        const auto node=value.toObject();const int id=node.value("id").toInt(-1);
        if(id<0 || ids.contains(id))return false;
        ids.insert(id);
    }
    return true;
}
static bool decode_preset_presentation(const QJsonValue& presentation,QJsonObject& checked) {
    return presentation.isUndefined() || (presentation.isObject() &&
        desktop::decode_numeric_preset_presentation(
            QJsonDocument(presentation.toObject()).toJson(QJsonDocument::Compact),checked));
}
bool SchemeEditorWidget::load_preset(const QString &id) {
    if (_application.graph_request_pending()) return false;
    for (const auto &value : std::as_const(_application.state().file_configs.ui.pipeline_presets)) {
        const auto preset=value.toObject();
        if (preset.value("id").toString()!=id || preset.value("version").toInt()!=1) continue;
        return load_selected_preset(preset);
    }
    return false;
}
bool SchemeEditorWidget::load_selected_preset(const QJsonObject& preset) {
    QJsonObject checkedPresentation;
    if(!decode_preset_presentation(preset.value("presentation"),checkedPresentation))return false;
    const auto document=preset.value("graph").toObject();
    if(!valid_preset_graph(document))return false;
    if(!load_document(document,preset.value("view").toObject()))return false;
    emit preset_presentation_requested(checkedPresentation);
    return true;
}
static bool graph_uses_capture(const domain::NumericProcessingGraph& graph) {
    for(const auto& node:graph.nodes) {
        if(node.kind!="AudioSource")continue;
        if(std::any_of(graph.edges.begin(),graph.edges.end(),[&](const auto& edge){
            return edge.from==node.id;
        }))return true;
    }
    return false;
}
static const char* capture_status_label(pipeline::State state,
    const std::optional<domain::NumericProcessingGraph>& active) {
    switch(state) {
    case pipeline::State::Starting:return "Capture starting";
    case pipeline::State::Running:return "Audio capture: receiving";
    case pipeline::State::Recovering:return "Capture recovering";
    case pipeline::State::Failed:return "Capture failed";
    case pipeline::State::Stopped:
        return active && !graph_uses_capture(*active)?"Audio input: not used by this graph":"Audio capture: inactive";
    }
    return nullptr;
}
void SchemeEditorWidget::refresh_capture_status() {
    core::perf::Scope profile(core::perf::Stage::CaptureStatus);
    if(!_capture_status)return;
    const char* label=capture_status_label(pipeline::State(_capture_state),_application.applied_numeric_graph());
    const auto text=label?ui::text(label):QString{};
    _capture_status->setText(text);
    _capture_status->setToolTip(ui::text("Audio input status. Processing continues independently; this indicator is not a start/stop control.")+(_capture_error.isEmpty()?QString{}:QStringLiteral("\n")+_capture_error));
    _capture_status->setVisible(!text.isEmpty());
}
void SchemeEditorWidget::schedule_apply() {
    if(_immediate_apply_pending && _application.graph_request_pending()){
        _apply_timer.stop();return; // The outstanding reply schedules the latest discrete edit.
    }
    _apply_timer.start(_immediate_apply_pending?0:350);
}
void SchemeEditorWidget::apply() {
    _apply_timer.stop();
    if(_application.graph_request_pending()){schedule_apply();return;}
    domain::NumericProcessingGraph graph;QString error;
    if(!numeric_draft(graph,&error)){set_status(ui::text("Not applied: ")+error);return;}
    _application.state().editor_draft_modified=true;
    emit numeric_draft_changed();
    set_status({});
    _apply_request=_application.apply_pipeline_graph(document());
}
void SchemeEditorWidget::wire_source_model(QtNodes::NodeId id) {
    if(auto *node=_application.state().data_flow_graph->delegateModel<AudioNodeModel>(id)){
        connect(node,&AudioNodeModel::activity_changed,this,[this,id]{if(auto* item=_scene->nodeGraphicsObject(id))item->update();});
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
    if(processing!=_last_processing_document)record_processing_edit(processing);
    if(adapters_changed)record_adapter_edit();
}
void SchemeEditorWidget::record_processing_edit(const QJsonObject& processing) {
    _live_apply_pending=true;_immediate_apply_pending=false;
    _last_processing_document=processing;
    _apply_timer.stop();
    _apply_request=0; // An older acknowledgement cannot label this draft active.
    clear_node_previews();
    sync_preview_subscription();
    QString error;
    domain::NumericProcessingGraph draft;
    const bool valid=numeric_draft(draft,&error);
    set_status(valid ? QString{} : ui::text("Not applied: ")+error);
    if(_unsupported_document)
        set_status(ui::text("Draft contains unsupported nodes. Original nodes and connections are preserved; automatic apply is disabled."));
    else if(valid)schedule_apply();
}
void SchemeEditorWidget::record_adapter_edit() {
    _apply_request=0;sync_preview_subscription();
    if(!_unsupported_document){_live_apply_pending=true;schedule_apply();}
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
namespace {
class EditorGraphLayout {
public:
    EditorGraphLayout(DataFlowGraphModel& graph, DataFlowGraphicsScene& scene) : _graph(graph), _scene(scene) {}
    std::map<QtNodes::NodeId, QPointF> positions() {
        build_edges();
        assign_layers();
        std::map<int, std::vector<QtNodes::NodeId>> columns;
        for (const auto& [id, column] : _layer) columns[column].push_back(id);
        double x = 0;
        for (auto& [column, ids] : columns) x += place_column(ids, x) + 40;
        return std::move(_positions);
    }
private:
    void build_edges() {
        for (const auto id : _graph.allNodeIds()) { _incoming[id] = 0; _layer[id] = 0; }
        const auto connections = _graph.save().value("connections").toArray();
        for (const auto value : connections) add_edge(value.toObject());
    }
    void add_edge(const QJsonObject& edge) {
        const auto from = QtNodes::NodeId(edge.value("outNodeId").toInt());
        const auto to = QtNodes::NodeId(edge.value("inNodeId").toInt());
        const auto* source = _graph.delegateModel<AudioNodeModel>(from);
        if (source && source->configuration_role(QtNodes::PortType::Out, edge.value("outPortIndex").toInt())) return;
        if (!_incoming.contains(from) || !_incoming.contains(to)) return;
        ++_incoming[to];
        _next[from].push_back(to);
    }
    void assign_layers() {
        std::queue<QtNodes::NodeId> queue;
        for (const auto& [id, count] : _incoming) if (!count) queue.push(id);
        while (!queue.empty()) {
            const auto id = queue.front();
            queue.pop();
            for (const auto to : _next[id]) {
                _layer[to] = std::max(_layer[to], _layer[id] + 1);
                if (--_incoming[to] == 0) queue.push(to);
            }
        }
    }
    int rank(QtNodes::NodeId id) const {
        const auto* model = _graph.delegateModel<AudioNodeModel>(id);
        const auto category = model ? numeric_editor::category(model->name()) : QString();
        return category == "Sources" ? 0 : category == "Processors" ? 1 : 2;
    }
    double place_column(std::vector<QtNodes::NodeId>& ids, double x) {
        std::stable_sort(ids.begin(), ids.end(), [this](auto a, auto b) { return rank(a) < rank(b); });
        double y = 0, width = 0;
        for (const auto id : ids) if (auto* item = _scene.nodeGraphicsObject(id)) {
            const auto bounds = item->boundingRect();
            _positions[id] = QPointF(x - bounds.left(), y - bounds.top());
            width = std::max(width, bounds.width());
            y += bounds.height() + 40;
        }
        return width;
    }
    DataFlowGraphModel& _graph;
    DataFlowGraphicsScene& _scene;
    std::map<QtNodes::NodeId, int> _incoming, _layer;
    std::map<QtNodes::NodeId, std::vector<QtNodes::NodeId>> _next;
    std::map<QtNodes::NodeId, QPointF> _positions;
};
}

void SchemeEditorWidget::layout_nodes(bool undoable) {
    auto& graph = *_application.state().data_flow_graph;
    auto positions = EditorGraphLayout(graph, *_scene).positions();
    if (positions.empty()) return;
    if (undoable) _scene->undoStack().push(new ArrangeCommand(graph, std::move(positions)));
    else for (const auto& [id, position] : positions) graph.setNodeData(id, QtNodes::NodeRole::Position, position);
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
