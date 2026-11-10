#include "isolated_gui_platform.h"
#include "synthetic_engine_provider.h"
#include <core_app/gui_presenter.h>
#include <core_app/scheme_editor/editor_widget.h>
#include <core_app/scheme_editor/numeric_editor_model.h>
#include <core_app/scheme_editor/audio_node_model.h>
#include <core_app/scheme_editor/editor_canvas_geometry.h>
#include <core_app/workspace/ui_command_registry.h>
#include <barelief/ui_text.h>
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QApplication>
#include <QAction>
#include <QScrollArea>
#include <QSplitter>
#include <QLineEdit>
#include <QWheelEvent>
#include <QMouseEvent>
#include <QImage>
#include <QPointer>
#include <QLineF>
#include <QKeyEvent>
#include <QElapsedTimer>
#include <QThread>
#include <QTimer>
#include <QDialog>
#include <QMenu>
#include <QPlainTextEdit>
#include <QWidgetAction>
#include <QPushButton>
#include <QGraphicsProxyWidget>
#include <cmath>
#include <type_traits>
#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <cstdio>
#include <stdexcept>
static void require(bool ok,const char* text){if(!ok)throw std::runtime_error(text);}
namespace canvas = presentation::editor_canvas;

static QRectF arranged_bounds(const std::vector<canvas::Node>& nodes,const canvas::Positions& positions) {
    QRectF result;
    for(const auto& node:nodes)result=result.united(node.bounds.translated(positions.at(node.id)));
    return result;
}
static void nonoverlapping_layout(const std::vector<canvas::Node>& nodes,const canvas::Positions& positions) {
    require(positions.size()==nodes.size(),"Arrange lost a node");
    for(std::size_t i=0;i<nodes.size();++i)for(std::size_t j=i+1;j<nodes.size();++j)
        require(!nodes[i].bounds.translated(positions.at(nodes[i].id)).intersects(
            nodes[j].bounds.translated(positions.at(nodes[j].id))),"Arrange overlaps real node bounds");
}
static void compact_chain_layout() {
    std::vector<canvas::Node> nodes;
    std::vector<canvas::Edge> edges;
    for(std::uint32_t i=0;i<80;++i) {
        nodes.push_back({5000+i,QRectF(-12,-8,180+20*(i%3),100+10*(i%5))});
        if(i)edges.push_back({4999+i,5000+i});
    }
    QRectF wide,tall;
    for(auto viewport:{QSizeF(1200,600),QSizeF(600,1200)}) {
        const auto positions=canvas::arrange(nodes,edges,viewport);
        require(positions==canvas::arrange(nodes,edges,viewport),"Arrange is nondeterministic");
        nonoverlapping_layout(nodes,positions);
        const auto bounds=arranged_bounds(nodes,positions);
        require(bounds.width()<6000 && bounds.height()>500,"Long DAG remains one unbounded horizontal row");
        if(viewport.width()>viewport.height())wide=bounds;else tall=bounds;
    }
    require(wide.width()/wide.height()>tall.width()/tall.height(),"Arrange ignores viewport aspect");
}
static void compact_branched_layout() {
    std::vector<canvas::Node> nodes{{91,QRectF(-5,-7,260,150)},{99,QRectF(-5,-7,260,150)}};
    std::vector<canvas::Edge> edges;
    for(std::uint32_t i=1000;i<1040;++i) {
        nodes.push_back({i,QRectF(-5,-7,160+20*(i%4),100+10*(i%3))});
        edges.push_back({91,i});edges.push_back({i,99});
    }
    auto positions=canvas::arrange(nodes,edges,{1000,700});
    nonoverlapping_layout(nodes,positions);
    const auto bounds=arranged_bounds(nodes,positions);
    require(bounds.height()<3500 && bounds.width()<5000,"Wide branches were not compacted");
    edges.push_back({99,91});
    nonoverlapping_layout(nodes,canvas::arrange(nodes,edges,{1000,700}));
    nonoverlapping_layout(nodes,canvas::arrange(nodes,{}, {1000,700}));
    require(canvas::arrange({},edges,{0,0}).empty(),"Empty layout created nodes");
}
static void bounded_canvas_geometry() {
    const QRectF nodes(-40000,-20000,130000,70000);
    const QSizeF viewport(800,600);
    const double scale=canvas::fit_scale(nodes,viewport);
    require(scale>0 && scale<0.2 && canvas::minimum_scale(nodes,viewport)<scale,
        "Large graphs cannot zoom out enough to fit");
    const auto bounds=canvas::navigation_bounds(nodes,viewport,scale);
    require(bounds.contains(nodes),"Canvas excludes remote nodes");
    auto center=canvas::constrain_center(bounds,viewport,scale,{1e9,-1e9});
    for(int i=0;i<100;++i)center=canvas::constrain_center(bounds,viewport,scale,center+QPointF(1e9,-1e9));
    require(center.x()<=bounds.right() && center.y()>=bounds.top(),"Repeated pan escapes finite canvas");
    const auto small=canvas::navigation_bounds({0,0,300,200},viewport,1);
    require(small.width()<2000 && small.height()<2000,"Canvas retains obsolete remote node extents");
    require(canvas::navigation_bounds({}, {0,0},1).isValid(),"Empty canvas is invalid");
}
static QRectF scene_node_bounds(QtNodes::DataFlowGraphicsScene* scene,const NumericEditorGraphModel& graph) {
    QRectF result;
    for(auto id:graph.allNodeIds())result=result.united(scene->nodeGraphicsObject(id)->sceneBoundingRect());
    return result;
}
static QPoint empty_canvas_point(QtNodes::GraphicsView* view) {
    for(int y=20;y<view->viewport()->height()-120;y+=20)
        for(int x=20;x<view->viewport()->width()-120;x+=20)
            if(view->items(QPoint(x,y)).empty())return {x,y};
    throw std::runtime_error("Fixture has no empty canvas for left drag");
}
static void canvas_mouse(QtNodes::GraphicsView* view,QEvent::Type type,QPoint point,
                         Qt::MouseButton button,Qt::MouseButtons buttons) {
    QMouseEvent event(type,point,view->viewport()->mapToGlobal(point),button,buttons,Qt::NoModifier);
    QApplication::sendEvent(view->viewport(),&event);
}
static void bounded_left_drag(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,
                              QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene) {
    const auto before=editor.document();
    const auto bounds=view->sceneRect();
    const auto center=view->mapToScene(view->viewport()->rect().center());
    for(int i=0;i<12;++i) {
        const auto from=empty_canvas_point(view),to=from+QPoint(100,80);
        canvas_mouse(view,QEvent::MouseButtonPress,from,Qt::LeftButton,Qt::LeftButton);
        canvas_mouse(view,QEvent::MouseMove,to,Qt::NoButton,Qt::LeftButton);
        canvas_mouse(view,QEvent::MouseButtonRelease,to,Qt::LeftButton,Qt::NoButton);
        QCoreApplication::processEvents();
        require(view->sceneRect()==bounds && bounds.contains(scene_node_bounds(scene,graph)),
            "Empty left drag translated canvas bounds away from nodes");
    }
    require(QLineF(center,view->mapToScene(view->viewport()->rect().center())).length()>5/view->getScale(),
        "Empty left drag stopped panning");
    require(editor.document()==before,"Empty left drag changed node positions or processing");
    for(auto id:graph.allNodeIds()) {
        view->centerOn(scene->nodeGraphicsObject(id));
        require(view->mapToScene(view->viewport()->rect()).containsPoint(
            scene->nodeGraphicsObject(id)->sceneBoundingRect().center(),Qt::OddEvenFill),
            "Left drag made a node unreachable");
    }
}
static void live_canvas_bounds(SchemeEditorWidget& editor,NumericEditorGraphModel& graph,
                              QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene) {
    const auto first=graph.nodeData(1,QtNodes::NodeRole::Position).toPointF();
    const auto second=graph.nodeData(2,QtNodes::NodeRole::Position).toPointF();
    graph.setNodeData(1,QtNodes::NodeRole::Position,QPointF(-60000,-20000));
    graph.setNodeData(2,QtNodes::NodeRole::Position,QPointF(90000,30000));
    QCoreApplication::processEvents();
    const auto nodes=scene_node_bounds(scene,graph);
    require(view->sceneRect().contains(nodes),"Moved nodes lie outside navigable canvas");
    for(auto id:graph.allNodeIds()) {
        view->centerOn(scene->nodeGraphicsObject(id));
        require(view->mapToScene(view->viewport()->rect()).containsPoint(
            scene->nodeGraphicsObject(id)->sceneBoundingRect().center(),Qt::OddEvenFill),
            "A moved node cannot be reached by camera");
    }
    auto* fit=editor.findChild<QPushButton*>("fitPipeline");require(fit,"Fit control missing");fit->click();
    const auto visible=view->mapToScene(view->viewport()->rect()).boundingRect();
    require(view->getScale()<0.2 && visible.adjusted(-4/view->getScale(),-4/view->getScale(),
        4/view->getScale(),4/view->getScale()).contains(nodes),"Fit clips a large real graph");
    const auto canvasBounds=view->sceneRect();
    for(int i=0;i<10;++i)view->centerOn(1e9,1e9);
    require(view->sceneRect()==canvasBounds,"Pan expands empty canvas without bound");
    graph.setNodeData(1,QtNodes::NodeRole::Position,first);
    graph.setNodeData(2,QtNodes::NodeRole::Position,second);
    QCoreApplication::processEvents();
    fit->click();QCoreApplication::processEvents();
    require(view->sceneRect().width()<10000,"Canvas did not shrink after remote nodes moved back");
    const auto added=graph.addNode("NetworkInput");
    graph.setNodeData(added,QtNodes::NodeRole::Position,QPointF(180000,120000));
    QCoreApplication::processEvents();
    require(view->sceneRect().contains(scene->nodeGraphicsObject(added)->sceneBoundingRect()),
        "Added node did not expand canvas");
    graph.deleteNode(added);QCoreApplication::processEvents();fit->click();
    require(view->sceneRect().width()<10000,"Deleted node retains obsolete canvas extent");
}
static void grid_palette_case(QtNodes::GraphicsView* view,const QString& theme) {
ui::Theme::instance().set(theme);QCoreApplication::processEvents();view->centerOn(90000,90000);QImage pixels(view->viewport()->size(),QImage::Format_ARGB32);view->viewport()->render(&pixels);const auto base=QApplication::palette().color(QPalette::Window);int different=0;for(int y=0;y<pixels.height();++y)for(int x=0;x<pixels.width();++x){const auto color=pixels.pixelColor(x,y);if(color!=base){++different;require(std::abs(color.lightness()-base.lightness())<=30,"Grid has stale palette or excessive contrast");}}require(different<pixels.width()*pixels.height()/8,"Editor grid is too dense");}

static void editor_grid_palette(QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene) {
        // Isolate the background oracle: bounded panning can leave nodes in view.
        QList<QGraphicsItem*> paintedItems;for(const auto items=scene->items();auto* item:items)if(!item->parentItem() && item->isVisible()){paintedItems.append(item);item->hide();}
        for(const auto& theme:{QString("light"),QString("dark")}){grid_palette_case(view,theme);}

        for(auto* item:paintedItems)item->show();
}

static void camera_resize_restore(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,const auto& center,const auto& near) {
        editor.flush_presentation();const auto resizeCenter=center();const auto resizeScale=view->getScale();
        for(const auto size:{QSize(800,600),QSize(1300,900),QSize(1100,760)}){editor.hide();editor.resize(size);editor.show();QTest::qWait(10);if(!near(resizeCenter) || view->getScale()!=resizeScale)std::fprintf(stderr,"Camera resize %dx%d desired=(%.3f,%.3f)/%.4f actual=(%.3f,%.3f)/%.4f viewport=%dx%d\n",size.width(),size.height(),resizeCenter.x(),resizeCenter.y(),resizeScale,center().x(),center().y(),view->getScale(),view->viewport()->width(),view->viewport()->height());require(near(resizeCenter) && view->getScale()==resizeScale,"Hide/show/resize resets camera");}
}

static void wait_for_keyboard_pan(const auto& center,QPointF origin) {
    const bool moved=QTest::qWaitFor([&] {return QLineF(origin,center()).length()>8;},1000);
    require(moved,"Keyboard pan timer did not advance camera");
}

static void held_keyboard_navigation(QtNodes::GraphicsView* view,const auto& center,const auto& near,const auto& key) {
        view->setFocus(Qt::OtherFocusReason);const auto beforeHeld=center();key(QEvent::KeyPress,Qt::Key_W);require(near(beforeHeld,2),"WASD press jumps in chunks");wait_for_keyboard_pan(center,beforeHeld);const auto held=center();require(held.y()<beforeHeld.y()-5,"Held W did not pan");key(QEvent::KeyRelease,Qt::Key_W,0,true);wait_for_keyboard_pan(center,held);require(center().y()<held.y()-3,"Autorepeat release stopped held key");key(QEvent::KeyRelease,Qt::Key_W);const auto released=center();QTest::qWait(45);require(near(released,2),"Key release fails to stop pan");
        // A delayed event loop must not turn direction checks into zero-tick checks.
        const auto diagonalStart=center();key(QEvent::KeyPress,Qt::Key_W);key(QEvent::KeyPress,Qt::Key_D);QTimer::singleShot(0,view,[]{QThread::msleep(100);});wait_for_keyboard_pan(center,diagonalStart);key(QEvent::KeyRelease,Qt::Key_W);key(QEvent::KeyRelease,Qt::Key_D);const auto delta=center()-diagonalStart;std::fprintf(stderr,"Diagonal delta=(%.3f,%.3f)\n",delta.x(),delta.y());require(delta.x()>5 && delta.y()<-5 && std::abs(delta.x()/delta.y())>.6 && std::abs(delta.x()/delta.y())<1.5,"Diagonal pan not normalized");
        constexpr quint32 physicalW=
#ifdef Q_OS_LINUX
            25;
#else
            17;
#endif
        const auto physicalStart=center();key(QEvent::KeyPress,0x0426,physicalW);wait_for_keyboard_pan(center,physicalStart);key(QEvent::KeyRelease,0x0426,physicalW);require(center().y()<physicalStart.y()-5,"Physical W under RU layout does not pan");
}

static void canvas_input_navigation(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,QLineEdit* text,const auto& center,const auto& near) {
        const QPoint cursor(view->viewport()->width()/3,view->viewport()->height()/3);const auto anchor=view->mapToScene(cursor);const auto beforeWheelScale=view->getScale();QWheelEvent wheel(cursor,view->viewport()->mapToGlobal(cursor),{},QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);QApplication::sendEvent(view->viewport(),&wheel);std::fprintf(stderr,"Wheel before=%.5f after=%.5f anchorDelta=%.5f center=(%.3f,%.3f)\n",beforeWheelScale,view->getScale(),QLineF(anchor,view->mapToScene(cursor)).length(),center().x(),center().y());require(view->getScale()>beforeWheelScale && QLineF(anchor,view->mapToScene(cursor)).length()<4/view->getScale(),"Wheel zoom loses pointer anchor");
        const QPoint from=view->viewport()->rect().center(),to=from+QPoint(70,45);const auto beforePan=center();auto mouse=[&](QEvent::Type type,QPoint point,Qt::MouseButton button,Qt::MouseButtons buttons){QMouseEvent event(type,point,view->viewport()->mapToGlobal(point),button,buttons,Qt::NoModifier);QApplication::sendEvent(view->viewport(),&event);};mouse(QEvent::MouseButtonPress,from,Qt::MiddleButton,Qt::MiddleButton);mouse(QEvent::MouseMove,to,Qt::NoButton,Qt::MiddleButton);mouse(QEvent::MouseButtonRelease,to,Qt::MiddleButton,Qt::NoButton);require(QLineF(beforePan,center()).length()>40/view->getScale(),"Middle pan did not move camera");
        auto key=[&](QEvent::Type type,int code,quint32 scan=0,bool repeat=false){QKeyEvent event(type,code,Qt::NoModifier,scan,0,0,QString(),repeat);QApplication::sendEvent(view,&event);};
        held_keyboard_navigation(view,center,near,key);
        key(QEvent::KeyPress,Qt::Key_W);text->setFocus(Qt::OtherFocusReason);QTest::qWait(30);const auto textCenter=center();QTest::qWait(45);require(near(textCenter,2),"Text focus does not stop keyboard pan");view->setFocus(Qt::OtherFocusReason);key(QEvent::KeyPress,Qt::Key_W);editor.hide();QTest::qWait(25);const auto hiddenCenter=center();QTest::qWait(45);require(near(hiddenCenter,2),"Hidden editor keeps panning");editor.show();QTest::qWait(20);
}

static void camera_navigation(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene,QScrollArea* inspector,GuiPresenter& presenter,QSignalSpy& applied,const auto& center,const auto& near) {
        scene->clearSelection();scene->nodeGraphicsObject(1)->setSelected(true);emit scene->nodeSelected(1);QCoreApplication::processEvents();auto* text=inspector->findChild<QLineEdit*>("adapterStreamId");require(text,"Current inspector text input missing");const auto beforeTyping=editor.history().count();text->setFocus(Qt::OtherFocusReason);QTest::keyClick(text,Qt::Key_A,Qt::AltModifier);require(editor.history().count()==beforeTyping,"Arrange intercepts inspector input");
        view->setFocus(Qt::OtherFocusReason);view->centerOn(scene->nodeGraphicsObject(1));QElapsedTimer quiet;quiet.start();auto replies=applied.count();for(int i=0;i<300 && (quiet.elapsed()<500 || presenter.graph_request_pending());++i){QTest::qWait(10);if(applied.count()!=replies || presenter.graph_request_pending()){replies=applied.count();quiet.restart();}}require(!presenter.graph_request_pending() && quiet.elapsed()>=500,"Pending source edit before navigation");const auto graphBeforeNavigation=editor.document();const auto runtimeBeforeNavigation=presenter.client().snapshot();applied.clear();
        editor.flush_presentation();
        QSignalSpy navigationSaves(&editor,&SchemeEditorWidget::numeric_draft_changed);
        canvas_input_navigation(editor,view,text,center,near);
        editor.flush_presentation();editor.flush_presentation();
        require(navigationSaves.isEmpty(),"Camera navigation or repeated snapshot flush requests project persistence");
        editor_grid_palette(view,scene);
        // Offscreen hide/show leaves the fixture window inactive. Run keyboard
        // routing first, then independently verify camera restoration without activation.
        camera_resize_restore(editor,view,center,near);

        require(editor.document()==graphBeforeNavigation && applied.isEmpty() && presenter.client().snapshot().graph_revision==runtimeBeforeNavigation.graph_revision,"Navigation/theme changes graph or applies DSP");
}

static void owned_menu_history(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,presentation::UiCommandRegistry& commands,const auto& beforeArrangeDocument,const auto& arrangedDocument,int arrangedIndex) {
        // Keep the owned menu alive after dismissal: Qt hides a menu before it
        // dispatches its shared QAction, so the registry must retain its origin.
        {
            QMenu menu(&editor);auto* undo=commands.action("undoDataFlow");auto* redo=commands.action("redoDataFlow");menu.addAction(undo);menu.addAction(redo);
            bool hiddenOnTrigger=false;QSignalSpy undoSignals(undo,&QAction::triggered),redoSignals(redo,&QAction::triggered);
            QObject::connect(undo,&QAction::triggered,&menu,[&]{hiddenOnTrigger=!menu.isVisible();});
            view->setFocus(Qt::OtherFocusReason);menu.show();menu.activateWindow();QCoreApplication::processEvents();menu.setFocus(Qt::PopupFocusReason);QCoreApplication::processEvents();commands.refresh();
            require(undo->isEnabled() && commands.can_execute("undoDataFlow"),"Canvas-origin menu lost graph Undo context");
            menu.setActiveAction(nullptr);QTest::keyClick(&menu,Qt::Key_Down);require(menu.activeAction()==undo,"Menu Down did not select shared Undo QAction");QTest::keyClick(&menu,Qt::Key_Return);
            require(hiddenOnTrigger && undoSignals.count()==1 && editor.history().index()==arrangedIndex-1 && editor.document()==beforeArrangeDocument,"Dismissed menu failed to dispatch shared Undo exactly once");
            QCoreApplication::processEvents();editor.activateWindow();QCoreApplication::processEvents();view->setFocus(Qt::OtherFocusReason);commands.refresh();
            require(redo->isEnabled(),"Closing owned menu did not restore canvas Redo availability");
            menu.show();menu.activateWindow();QCoreApplication::processEvents();menu.setFocus(Qt::PopupFocusReason);QCoreApplication::processEvents();menu.setActiveAction(nullptr);QTest::keyClick(&menu,Qt::Key_Down);if(menu.activeAction()!=redo)QTest::keyClick(&menu,Qt::Key_Down);require(menu.activeAction()==redo,"Menu Down navigation did not select shared Redo QAction");QTest::keyClick(&menu,Qt::Key_Return);
            require(redoSignals.count()==1 && editor.history().index()==arrangedIndex && editor.document()==arrangedDocument,"Retained menu failed to dispatch shared Redo exactly once");
            QCoreApplication::processEvents();editor.activateWindow();QCoreApplication::processEvents();view->setFocus(Qt::OtherFocusReason);commands.refresh();
        }
}

static void text_history(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,presentation::UiCommandRegistry& commands,const auto& beforeArrangeDocument,const auto& arrangedDocument,int arrangedIndex) {
        auto check_local_history=[&](auto* input){
            input->show();input->window()->activateWindow();QCoreApplication::processEvents();input->setFocus(Qt::OtherFocusReason);QCoreApplication::processEvents();commands.refresh();
            QTest::keyClicks(input,"history");require(!commands.can_execute("undoDataFlow") && !commands.can_execute("redoDataFlow"),"Text focus leaves graph history enabled");
            QTest::keyClick(input,Qt::Key_Z,Qt::ControlModifier);
            if constexpr(std::is_same_v<std::remove_pointer_t<decltype(input)>,QLineEdit>) require(input->text().isEmpty(),"Line edit local Undo failed");
            else require(input->toPlainText().isEmpty(),"Plain text local Undo failed");
            QTest::keyClick(input,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);
            require(editor.history().index()==arrangedIndex && editor.document()==arrangedDocument,"Text history shortcut changes graph");
        };
        {QLineEdit input(&editor);check_local_history(&input);require(input.text()=="history","Line edit local Undo/Redo failed");}
        {QPlainTextEdit input(&editor);check_local_history(&input);require(input.toPlainText()=="history","Plain text local Undo/Redo failed");}
        view->setFocus(Qt::OtherFocusReason);
        auto restore_canvas_history=[&]{QCoreApplication::processEvents();editor.activateWindow();QCoreApplication::processEvents();view->setFocus(Qt::OtherFocusReason);commands.refresh();require(commands.action("undoDataFlow")->isEnabled(),"Popup/dialog dismissal left canvas Undo disabled");QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);require(editor.document()==beforeArrangeDocument && commands.action("redoDataFlow")->isEnabled(),"Restored canvas Undo/Redo unavailable");QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);require(editor.document()==arrangedDocument && editor.history().index()==arrangedIndex,"Restored canvas Redo failed");};
        {QMenu popup(&editor);auto* entry=new QWidgetAction(&popup);auto* input=new QLineEdit;entry->setDefaultWidget(input);popup.addAction(entry);popup.show();check_local_history(input);require(input->text()=="history","Popup line edit local Undo/Redo failed");popup.hide();restore_canvas_history();}
        {QDialog dialog(&editor);dialog.setWindowModality(Qt::WindowModal);QLineEdit input(&dialog);dialog.show();check_local_history(&input);require(input.text()=="history","Dialog line edit local Undo/Redo failed");dialog.hide();restore_canvas_history();}
}

static void inline_history(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene,presentation::UiCommandRegistry& commands,const auto& graph,const auto& arrangedDocument,int arrangedIndex) {
        editor.activateWindow();QCoreApplication::processEvents();view->setFocus(Qt::OtherFocusReason);commands.refresh();
        {
            auto* node=graph->template delegateModel<AudioNodeModel>(1);require(node && node->set_parameter_on_block("adapterStreamId",true),"Inline stream field could not be enabled");QCoreApplication::processEvents();
            auto* input=node->embeddedWidget()->template findChild<QLineEdit*>("adapterStreamId");require(input && input->isVisible(),"Actual node inline field missing");view->centerOn(scene->nodeGraphicsObject(1));view->setFocus(Qt::OtherFocusReason);input->setFocus(Qt::OtherFocusReason);QCoreApplication::processEvents();
            auto* proxy=node->embeddedWidget()->graphicsProxyWidget();
            require((QApplication::focusWidget()==view || view->isAncestorOf(QApplication::focusWidget())) && proxy && scene->focusItem() && (scene->focusItem()==proxy || proxy->isAncestorOf(scene->focusItem())) && node->embeddedWidget()->focusWidget()==input && input->hasFocus(),"Inline field did not use graphics-proxy focus path");
            const auto inlineDocument=editor.document();const auto inlineIndex=editor.history().index();const auto initial=input->text();input->end(false);QTest::keyClicks(input,"local");commands.refresh();
            require(!commands.can_execute("undoDataFlow") && !commands.action("undoDataFlow")->isEnabled(),"Inline node text leaves graph Undo enabled");
            QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);require(input->text()==initial && editor.document()==inlineDocument && editor.history().index()==inlineIndex,"Inline local Undo changed graph history");
            QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);require(input->text()==initial+"local" && editor.document()==inlineDocument && editor.history().index()==inlineIndex,"Inline local Redo changed graph history");
            QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);view->setFocus(Qt::OtherFocusReason);scene->clearFocus();QCoreApplication::processEvents();commands.refresh();require(commands.action("undoDataFlow")->isEnabled(),"Leaving inline field did not restore graph Undo");
            QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);require(editor.document()==arrangedDocument && editor.history().index()==arrangedIndex,"Canvas Undo failed to remove inline-field presentation edit");
        }}

static void history_shortcuts(SchemeEditorWidget& editor,const auto& graph,QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene,const auto& center) {
        presentation::UiCommandRegistry commands(&editor);commands.set_editor(&editor);
        commands.register_command({"undoDataFlow","Undo",QKeySequence("Ctrl+Z"),presentation::UiCommandContext::SchemaEditor,[&]{editor.history().undo();},[&]{return editor.history().canUndo();}});
        commands.register_command({"redoDataFlow","Redo",QKeySequence("Ctrl+Shift+Z"),presentation::UiCommandContext::SchemaEditor,[&]{editor.history().redo();},[&]{return editor.history().canRedo();}});
        QObject::connect(&editor.history(),&QUndoStack::indexChanged,&commands,&presentation::UiCommandRegistry::refresh);
        graph->setNodeData(1,QtNodes::NodeRole::Position,QPointF(21000,25000));graph->setNodeData(2,QtNodes::NodeRole::Position,QPointF(-2000,600));const auto beforeArrange=editor.history().count();const auto beforeArrangeDocument=editor.document();
        // Only this fixture's offscreen Qt widget receives focus/events. No OS
        // input, cursor positioning or desktop capture is used. QApplication's
        // active-window state below belongs exclusively to offscreen fixtures.
        view->setFocus(Qt::OtherFocusReason);QTest::qWait(10);require(view->hasFocus(),"Offscreen canvas Qt focus unavailable");QTest::keyClick(view,Qt::Key_A,Qt::AltModifier);QTest::qWait(10);require(editor.history().count()==beforeArrange+1,"Canvas Alt+A did not arrange once");
        require(scene->nodeGraphicsObject(1)->sceneBoundingRect().right()<scene->nodeGraphicsObject(2)->sceneBoundingRect().left(),"Arrange does not follow route direction");view->centerOn(90000,90000);QTest::keyClick(view,Qt::Key_F,Qt::AltModifier);require(QLineF(center(),QPointF(90000,90000)).length()>100,"Canvas Alt+F did not center");
        const auto arrangedDocument=editor.document();const auto arrangedIndex=editor.history().index();
        QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier);require(editor.document()==beforeArrangeDocument && editor.history().index()==arrangedIndex-1,"Canvas Ctrl+Z did not undo exactly once");
        QTest::keyClick(view,Qt::Key_Z,Qt::ControlModifier|Qt::ShiftModifier);require(editor.document()==arrangedDocument && editor.history().index()==arrangedIndex,"Canvas Ctrl+Shift+Z did not redo exactly once");
        owned_menu_history(editor,view,commands,beforeArrangeDocument,arrangedDocument,arrangedIndex);
        text_history(editor,view,commands,beforeArrangeDocument,arrangedDocument,arrangedIndex);
        inline_history(editor,view,scene,commands,graph,arrangedDocument,arrangedIndex);

}

static void inspector_camera_stability(SchemeEditorWidget& editor,QtNodes::GraphicsView* view,QtNodes::DataFlowGraphicsScene* scene,QScrollArea* inspector,QSplitter* splitter,const auto& center,const auto& near) {
        view->setupScale(.75);view->centerOn(1200,900);editor.flush_presentation();
        const auto selectionSize=view->viewport()->size();const auto selectionCenter=center();for(int i=0;i<5;++i){scene->clearSelection();scene->nodeGraphicsObject(i%2+1)->setSelected(true);emit scene->nodeSelected(i%2+1);QCoreApplication::processEvents();require(inspector->isVisible() && view->viewport()->size()==selectionSize && near(selectionCenter),"Selection changes inspector layout or canvas center");}
        scene->clearSelection();QCoreApplication::processEvents();require(inspector->findChild<QLabel*>("pipelineInspectorPlaceholder"),"Deselection does not restore inspector placeholder");
        auto* handle=splitter->handle(2);auto clickHandle=[&]{const auto point=handle->rect().center();for(const auto type:{QEvent::MouseButtonPress,QEvent::MouseButtonRelease}){QMouseEvent event(type,point,handle->mapToGlobal(point),Qt::LeftButton,type==QEvent::MouseButtonPress?Qt::LeftButton:Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(handle,&event);}QCoreApplication::processEvents();};const auto expandedCenter=center();clickHandle();require(splitter->sizes().at(2)==0 && near(expandedCenter),"Inspector collapse loses camera");scene->nodeGraphicsObject(1)->setSelected(true);emit scene->nodeSelected(1);QCoreApplication::processEvents();require(splitter->sizes().at(2)==0,"Selection reopens collapsed inspector");clickHandle();require(splitter->sizes().at(2)>0 && near(expandedCenter),"Inspector does not restore width/camera");
}

static void repeated_node_selection_keeps_form(NumericEditorGraphModel& graph,
    QtNodes::DataFlowGraphicsScene* scene,QScrollArea* inspector) {
    const auto nodes=graph.allNodeIds();require(!nodes.empty(),"Inspector regression needs a node");
    const auto id=*nodes.begin();Q_EMIT scene->nodeSelected(id);
    QPointer<QWidget> card=inspector->findChild<QWidget*>("pipelineDataStatus");
    require(card,"Selected node preview missing");
    Q_EMIT scene->nodeSelected(id);QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
    require(card && inspector->findChild<QWidget*>("pipelineDataStatus")==card,
        "Clicking an already selected node recreated its form and preview");
}
int main(int argc,char** argv){
    if(!test_fixture::isolated_gui_platform())return 2;
    QApplication app(argc,argv);
    try{
        bounded_canvas_geometry();compact_chain_layout();compact_branched_layout();
        GuiState state;state.file_configs.ui.autosave_enabled=false;state.file_configs.ui.pipeline_auto_apply=false;state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();for(const auto kinds=numeric_editor::model_catalog();const auto& kind:kinds)state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
        auto graph=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);state.data_flow_graph=graph;domain::NumericProcessingGraph typed;typed.graph_id="main";auto source=numeric_editor::default_node("NetworkInput"),sink=numeric_editor::default_node("NetworkOutput");source.id=1;sink.id=2;typed.nodes={source,sink};typed.edges={{1,2,0,0}};require(graph->set_numeric_graph(typed),"Canonical navigation graph rejected");state.file_configs.scene_graph=graph->save();
        GuiPresenter presenter(state,test_fixture::synthetic_engine_provider(false));SchemeEditorWidget editor(presenter);editor.resize(1100,760);editor.show();QTest::qWait(30);QSignalSpy applied(&presenter,&GuiPresenter::graph_apply_finished);
        auto* view=editor.findChild<QtNodes::GraphicsView*>();auto* scene=editor.findChild<QtNodes::DataFlowGraphicsScene*>();auto* inspector=editor.findChild<QScrollArea*>("pipelineInspector");auto* splitter=editor.findChild<QSplitter*>("pipelineSplitter");require(view && scene && inspector && splitter && splitter->count()==3,"Navigation components missing");
        auto center=[&]{return view->mapToScene(view->viewport()->rect().center());};auto near=[&](QPointF expected,double pixels=4){return QLineF(expected,center()).length()<pixels/view->getScale();};
        live_canvas_bounds(editor,*graph,view,scene);
        bounded_left_drag(editor,*graph,view,scene);
        inspector_camera_stability(editor,view,scene,inspector,splitter,center,near);
        repeated_node_selection_keeps_form(*graph,scene,inspector);
        history_shortcuts(editor,graph,view,scene,center);
        camera_navigation(editor,view,scene,inspector,presenter,applied,center,near);
        std::puts("canonical editor navigation: inspector/scoped shortcuts, camera resize/zoom/pan/RU physical keys, stop fences and central grid themes PASS");return 0;
    }catch(const std::exception& error){std::fprintf(stderr,"FAIL %s\n",error.what());return 1;}
}
