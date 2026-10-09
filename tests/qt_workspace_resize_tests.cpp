#include <QQuickItem>
#include "synthetic_engine_provider.h"
#include "numeric_gui_fixture.h"
#include "core_app/gui_presenter.h"
#include "core_app/main_window.h"
#include "core_app/graph_panel.h"
#include <QStackedWidget>
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/ui_text.h"
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QApplication>
#include <QAbstractEventDispatcher>
#ifdef Q_OS_WIN
#include <windows.h>
#endif
#include <QComboBox>
#include <DockAreaWidget.h>
#include <DockAreaTitleBar.h>
#include <QToolBar>
#include <FloatingDockContainer.h>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenuBar>
#include <QMenu>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStatusBar>
#include <QFile>
#include <QJsonDocument>
#include <QToolButton>
#include <QWheelEvent>
#include <QPointingDevice>
#include <QElapsedTimer>
#include <QContextMenuEvent>
#include <QTimer>
#include <core_app/graph_panel.h>
#include <QtTest/QTest>
#include <stdexcept>

static QRect rectInWindow(QWidget *widget) {
    if(auto *dock=qobject_cast<WorkspaceDock *>(widget); dock && dock->dockAreaWidget()) widget=dock->dockAreaWidget();
    return QRect(widget->mapTo(widget->window(),QPoint()),widget->size());
}

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void settle() { QCoreApplication::processEvents(); QTest::qWait(15); }

static GuiState fixture() {
    GuiState state;
    state.file_configs.core.run_loopback = false;
    state.file_configs.core.run_analyser = false;
    state.file_configs.ui.autosave_enabled = false;
    state.file_configs.ui.pipeline_auto_apply = false;
    state.file_configs.ui.show_fps = false;
    state.file_configs.ui.editor_view = {{"version", 1}, {"scale", .75}, {"center_x", 800}, {"center_y", 600}};
    const auto configured=numeric_test_graph();
    state.file_configs.scene_graph=gui_fixture::encoded(configured);
    state.file_configs.pipeline_graph=state.file_configs.scene_graph;
    state.delegate_model_registry=std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for(const auto& kind:numeric_editor::catalog())state.delegate_model_registry->registerModel([kind]{return std::make_unique<AudioNodeModel>(kind);},numeric_editor::category(kind));
    auto model=std::make_shared<NumericEditorGraphModel>(state.delegate_model_registry);
    require(model->set_numeric_graph(configured),"canonical model fixture");state.data_flow_graph=model;
    return state;
}

static void inspector_selection(GuiPresenter &presenter) {
    SchemeEditorWidget editor(presenter);
    editor.resize(1100, 600); editor.show(); settle();
    auto *splitter = editor.findChild<QSplitter *>("pipelineSplitter");
    auto *scene = editor.findChild<QtNodes::DataFlowGraphicsScene *>();
    auto *view = editor.findChild<QtNodes::GraphicsView *>();
    require(splitter && scene && view, "Editor components absent");
    require(editor.findChild<QLabel *>("pipelineInspectorPlaceholder"), "Empty selection has no placeholder");
    splitter->setSizes({170, 660, 246}); settle();
    const auto widths = splitter->sizes();
    const auto center = view->mapToScene(view->viewport()->rect().center());
    const auto scale = view->getScale();
    for (auto id : {QtNodes::NodeId(1), QtNodes::NodeId(10), QtNodes::NodeId(1)}) {
        scene->clearSelection(); scene->nodeGraphicsObject(id)->setSelected(true); emit scene->nodeSelected(id); settle();
        require(splitter->sizes() == widths, "Node selection changed workspace widths");
        require(QLineF(center, view->mapToScene(view->viewport()->rect().center())).length() < 2 / scale,
                "Node selection moved editor camera");
    }
    auto *handle = splitter->handle(2);
    QTest::mouseClick(handle, Qt::LeftButton); settle();
    require(splitter->sizes()[2] == 0, "Divider click did not collapse inspector");
    const auto collapsed = splitter->sizes();
    scene->clearSelection(); scene->nodeGraphicsObject(10)->setSelected(true); emit scene->nodeSelected(10); settle();
    require(splitter->sizes() == collapsed, "Node selection reopened collapsed inspector");
    QTest::mouseClick(handle, Qt::LeftButton); settle();
    require(std::abs(splitter->sizes()[2] - widths[2]) <= 2, "Divider click did not restore inspector width");
    scene->clearSelection(); settle();
    require(editor.findChild<QLabel *>("pipelineInspectorPlaceholder"), "Deselecting lost inspector placeholder");
    splitter->setSizes({170, 740, 165}); settle();
    const int narrow_width = splitter->sizes()[2];
    QTest::mouseClick(handle, Qt::LeftButton); settle();
    QTest::mouseClick(handle, Qt::LeftButton); settle();
    require(std::abs(splitter->sizes()[2] - narrow_width) <= 2, "Narrow inspector width was replaced by a fixed minimum");
}

static void narrow_controls(GuiPresenter &presenter) {
    SchemeEditorWidget editor(presenter);
    editor.resize(800, 420); editor.show(); settle();
    auto *splitter = editor.findChild<QSplitter *>("pipelineSplitter");
    auto *scene = editor.findChild<QtNodes::DataFlowGraphicsScene *>();
    auto *inspector = editor.findChild<QScrollArea *>("pipelineInspector");
    for(const auto *theme : {"light","dark"}) {
    ui::Theme::instance().set(QString::fromLatin1(theme));
    for (const auto &node : presenter.state().file_configs.scene_graph.value("nodes").toArray()) {
        const auto id=QtNodes::NodeId(node.toObject().value("id").toInt());
        scene->clearSelection(); scene->nodeGraphicsObject(id)->setSelected(true); emit scene->nodeSelected(id);
        splitter->setSizes({140, 475, 165}); settle();
        auto *title=editor.findChild<QLabel *>("pipelineInspectorTitle");
        require(title, "Inspector title absent");
        const QRect title_bounds(title->mapTo(inspector->widget(),QPoint()),title->size());
        if(title_bounds.right()>=inspector->viewport()->width()) {
            qWarning()<<"Narrow title"<<id<<title->text()<<title_bounds<<"viewport"<<inspector->viewport()->size()<<"contents"<<inspector->widget()->size()<<"minimum"<<inspector->widget()->minimumSizeHint();
            for(auto* child:inspector->widget()->findChildren<QWidget*>())
                if(child->isVisible())qWarning()<<"Narrow minimum"<<child->metaObject()->className()<<child->objectName()<<child->size()<<child->minimumSize()<<child->minimumSizeHint()<<child->sizePolicy();
        }
        require(title_bounds.right()<inspector->viewport()->width(),"Inspector title exceeds narrow viewport");
        auto *parameters = inspector->widget()->findChild<QWidget *>("numericNodeParameters");
        require(parameters, "Inspector parameters absent");
        for (auto *control : parameters->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!control->isVisible()) continue;
            if(!parameters->rect().contains(control->geometry()))
                qWarning()<<"Parameter bounds"<<id<<control->metaObject()->className()<<control->objectName()<<control->geometry()<<parameters->rect();
            require(parameters->rect().contains(control->geometry()), "Narrow inspector control escaped parameter bounds");
            const QRect bounds(control->mapTo(inspector->widget(), QPoint()), control->size());
            if (bounds.right() >= inspector->viewport()->width())
            {
                qWarning() << "Node" << id << control->objectName() << bounds << "viewport" << inspector->viewport()->size()
                           << "contents" << inspector->widget()->size();
                for (auto *child : inspector->widget()->findChildren<QWidget *>(QString(),Qt::FindDirectChildrenOnly))
                    qWarning() << child->objectName() << child->minimumSize() << child->minimumSizeHint() << child->sizePolicy();
            }
            require(bounds.right() < inspector->viewport()->width(), "Narrow inspector control exceeds viewport");
        }
    }
    }
}

static void dataflow_footer(GuiPresenter &presenter) {
    MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph);
    // Finish initial engine snapshot/navigation delivery before the user closes
    // graph areas. Otherwise the bootstrap migration legitimately opens them.
    settle();
    auto *flow = window.workspaceDock("dataFlowDock");
    require(flow, "Data Flow dock absent");
    for (auto *dock : window.workspaceDocks()) if (dock != flow) dock->toggleView(false);
    window.resize(800, 600); window.show(); settle();
    for (const auto size : {QSize(800,600), QSize(1200,760), QSize(800,600)}) {
        window.resize(size); settle();
        require(rectInWindow(flow).bottom() < (window.statusBar()->isVisible() ? window.statusBar()->geometry().top() : window.height()), "Data Flow intersects footer");
        require(rectInWindow(flow).left() <= 1 && rectInWindow(flow).right() >= window.width()-2,
                "Only Data Flow retained an empty dock splitter area");
        if (rectInWindow(flow).top() > window.menuBar()->geometry().bottom()+2)
        {
            qWarning() << "flow" << rectInWindow(flow) << "menu" << window.menuBar()->geometry() << "footer" << window.statusBar()->geometry();
            for (auto *dock : window.workspaceDocks()) qWarning() << dock->objectName() << dock->isHidden() << rectInWindow(dock);
        }
        require(rectInWindow(flow).top() <= window.menuBar()->geometry().bottom()+2,
                "Only Data Flow retained an empty upper dock area");
        for (auto *control : {window.findChild<QWidget *>("pipelineToggle"), window.findChild<QWidget *>("themeMenuButton")}) {
            require(control, "Footer control absent");
            const QRect bounds(control->mapTo(&window,QPoint()),control->size());
            require(window.menuBar()->geometry().contains(bounds), "Navigation control escapes upper bar");
        }
    }
    window.close();
}

static void restored_startup(GuiPresenter &presenter) {
    // Restore a saved tabbed workspace before the first event loop, as the
    // desktop entrypoint does, then close and restore its resulting layout.
    for (int pass = 0; pass < 4; ++pass) {
        MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph);
        auto *fft = window.workspaceDock("fftDock");
        auto *mel = window.workspaceDock("melDock");
        require(fft && mel, "Saved workspace graph docks absent");
        if (pass == 0) presenter.initialize();
        window.show();
        QTest::qWait(250); // Includes deferred dock animation deletion.
        require(window.isVisible(), "Restored workspace did not remain open");
        window.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

static void compact_chrome(GuiPresenter &presenter) {
    MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph); window.resize(1000,700); window.show(); presenter.initialize(); settle();
    for(const auto *theme:{"dark","light"}) {
        ui::Theme::instance().set(QString::fromLatin1(theme)); settle();
        for(const auto size:{QSize(800,600),QSize(1200,760)}) {
        window.resize(size); settle();
        for(const auto *id:{"pipelinePresets","pipelineToggle","themeMenuButton","engineSelectorButton"}) {
            auto *button=window.findChild<QToolButton *>(id);
            require(button && button->height()==24 && button->property("compactControl").toBool(),"Navigation button does not share compact chrome");
            const QRect bounds(button->mapTo(&window,QPoint()),button->size());
            require(window.menuBar()->geometry().contains(bounds),"Navigation button is outside upper bar");
            if(button->menu()) require(button->property("menuControl").toBool(),"Menu button is missing shared arrow chrome");
            require(button->styleSheet().isEmpty(),"Navigation button has a private style override");
        }
        }
        auto *fps=window.findChild<QLabel *>("renderStatus");
        require(!fps,"Main window still shows aggregate FPS");
        require(!window.statusBar()->isVisible(),"Empty footer still takes workspace height");
    }
    auto *editor=window.findChild<SchemeEditorWidget *>();
    const auto before=window.workspaceDocks();emit editor->graph_open_requested(8,0);settle();
    WorkspaceDock* surface=nullptr;for(auto* dock:window.workspaceDocks())if(!before.contains(dock))surface=dock;
    require(surface && surface->isFloating() && surface->floatingDockContainer(),"external graph container");
    auto* renderer=surface->widget();surface->findChild<QAction*>("graphPlacement")->menu()->actions()[1]->trigger();settle();
    require(!surface->isFloating() && surface->widget()==renderer,"return recreates renderer");window.close();surface->close();

}

static void scoped_windows(GuiPresenter &presenter) {
    MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph);window.resize(1500,900);window.show();settle();
    auto* editor=window.findChild<SchemeEditorWidget*>();auto* flow=window.workspaceDock("dataFlowDock");
    for(auto* dock:window.workspaceDocks())if(dock!=flow)dock->toggleView(false);
    emit editor->graph_view_requested(10);settle();
    for(auto* dock:window.workspaceDocks())if(dock!=flow)require(dock->isClosed(),"zero slots created graph on focus");
    QList<WorkspaceDock*> created;
    for(const auto spec:{std::pair{10,2},std::pair{11,2},std::pair{10,0},std::pair{8,0}}){
        const auto before=window.workspaceDocks();QList<WorkspaceDock*> closed;
        for(auto* dock:before)if(dock->isClosed() && dock->property("graphNode").isValid() && dock->property("graphNode").toInt()==spec.first)closed.append(dock);
        emit editor->graph_open_requested(spec.first,spec.second);settle();
        WorkspaceDock* added=nullptr;for(auto* dock:window.workspaceDocks())if(!before.contains(dock))added=dock;
        if(!added)for(auto* dock:closed)if(!dock->isClosed())added=dock;
        require(added && !added->isClosed(),"explicit open creates or reopens closed instance");created.append(added);
    }
    require(created[0]->widget()!=created[2]->widget() && created[2]->isFloating() && created[3]->isFloating(),"independent internal/external views");
    auto* position=window.findChild<QComboBox*>("dataFlowPosition");require(position,"flow position selector");
    for(int value:{1,2,3,0}){position->setCurrentIndex(value);settle();for(auto* dock:created)if(!dock->isFloating())require(!rectInWindow(flow).intersects(rectInWindow(dock)),"flow overlaps internal graph");}
    created[0]->toggleView(false);created[0]->toggleView(true);settle();require(!created[0]->isClosed(),"hide/reopen instance");
    window.close();for(auto* dock:window.workspaceDocks())dock->close();
}

static void graph_docking(GuiPresenter &presenter) {
    MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph);
    window.resize(1200,850); window.show(); presenter.initialize(); QTest::qWait(100);
    auto *flow=window.workspaceDock("dataFlowDock");
    auto *fft=window.workspaceDock("fftDock");
    auto *mel=window.workspaceDock("melDock");
    auto *editor=window.findChild<SchemeEditorWidget *>();
    require(flow && fft && mel && editor,"Placement fixture missing");
    require(flow->headerControls()==editor->toolbar() && flow->dockAreaWidget()->titleBar()->isAncestorOf(editor->toolbar()),"Data Flow toolbar retained separate row");
    require(flow->headerControls()->findChild<QDoubleSpinBox *>("pipelineZoom"),"Header zoom absent");
    auto *canvas=editor->findChild<QGraphicsView *>();
    editor->center_action()->trigger(); settle();
    const auto beforePan=canvas->mapToScene(canvas->viewport()->rect().center());
    const auto beforeTransform=canvas->transform();
    const auto pointer=QPointF(canvas->viewport()->rect().center());
    QWheelEvent pan(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(0,36),QPoint(),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
    QApplication::sendEvent(canvas->viewport(),&pan);
    require(canvas->transform()==beforeTransform && QLineF(beforePan,canvas->mapToScene(canvas->viewport()->rect().center())).length()>10,
            "Two-finger pixel scrolling zooms instead of panning");
    const auto pixelCenter=canvas->mapToScene(canvas->viewport()->rect().center());
    QWheelEvent windowsPan(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
    QApplication::sendEvent(canvas->viewport(),&windowsPan);
    require(canvas->transform()==beforeTransform && QLineF(pixelCenter,canvas->mapToScene(canvas->viewport()->rect().center())).length()>10,
            "Windows angle-only touchpad scrolling did not pan");
    require(canvas->horizontalScrollBarPolicy()==Qt::ScrollBarAsNeeded && canvas->verticalScrollBarPolicy()==Qt::ScrollBarAsNeeded,
            "Canvas scrollbars are disabled");
    QPointingDevice touchpad("test precision touchpad",9876,QInputDevice::DeviceType::TouchPad,QPointingDevice::PointerType::Finger,
                            QInputDevice::Capability::Position,2,0);
    const auto swipeTransform=canvas->transform();
    const auto beforeSwipe=canvas->mapToScene(canvas->viewport()->rect().center());
    QWheelEvent plainSwipe(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false,Qt::MouseEventNotSynthesized,&touchpad);
    QApplication::sendEvent(canvas->viewport(),&plainSwipe);
    require(canvas->transform()==swipeTransform && canvas->mapToScene(canvas->viewport()->rect().center()).y()>beforeSwipe.y()+5,
            "Angle-only vertical touchpad swipe zooms instead of panning");
    QWheelEvent pinch(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::ControlModifier,Qt::NoScrollPhase,false,Qt::MouseEventNotSynthesized,&touchpad);
    QApplication::sendEvent(canvas->viewport(),&pinch);
    require(canvas->transform()!=swipeTransform,"Touchpad pinch pans instead of zooming");
// Native Windows event-filter probe is retained in audit source; synthetic Qt events above are isolated.

    const auto beforeMouseZoom=canvas->transform();
    QWheelEvent zoomWheel(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,120),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(canvas->viewport(),&zoomWheel);
    require(canvas->transform()!=beforeMouseZoom,"Unmodified mouse wheel no longer zooms");
    const auto zoomTransform=canvas->transform();
    const auto beforeCtrl=canvas->mapToScene(canvas->viewport()->rect().center());
    QWheelEvent ctrlWheel(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::ControlModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(canvas->viewport(),&ctrlWheel);
    const auto afterCtrl=canvas->mapToScene(canvas->viewport()->rect().center());
    require(canvas->transform()==zoomTransform && std::abs(afterCtrl.x()-beforeCtrl.x())<2 && afterCtrl.y()>beforeCtrl.y()+5,"Ctrl wheel does not pan only vertically");
    QWheelEvent altWheel(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(),QPoint(0,-120),Qt::NoButton,Qt::AltModifier,Qt::NoScrollPhase,false);
    QApplication::sendEvent(canvas->viewport(),&altWheel);
    const auto afterAlt=canvas->mapToScene(canvas->viewport()->rect().center());
    require(canvas->transform()==zoomTransform && std::abs(afterAlt.y()-afterCtrl.y())<2 && afterAlt.x()>afterCtrl.x()+5,"Alt wheel does not pan only horizontally");
    QWheelEvent runaway(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(1000000,1000000),QPoint(),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
    QApplication::sendEvent(canvas->viewport(),&runaway);
    const auto stoppedCenter=canvas->mapToScene(canvas->viewport()->rect().center());
    require(canvas->sceneRect().contains(stoppedCenter) && canvas->sceneRect().width()<100000,
            "Panning can escape into an unbounded empty scene");
    QWheelEvent momentum(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),QPoint(-100,-100),QPoint(),Qt::NoButton,Qt::NoModifier,Qt::ScrollMomentum,false);
    QApplication::sendEvent(canvas->viewport(),&momentum);
    require(QLineF(stoppedCenter,canvas->mapToScene(canvas->viewport()->rect().center())).length()<1,
            "Touchpad momentum continued panning after release");
    const auto revision=presenter.client().snapshot().graph_revision;
    auto* surface=window.workspaceDock("spectrogramDock");auto* content=surface->widget();
    for(int i=0;i<3;++i){surface->setAutoHide(true,ads::SideBarRight);settle();require(surface->isAutoHide(),"sidebar pin failed");surface->setAutoHide(false);surface->raise();settle();require(surface->widget()==content,"pin recreated renderer");}
    auto* scene=editor->findChild<QtNodes::DataFlowGraphicsScene*>();auto* model=dynamic_cast<NumericEditorGraphModel*>(&scene->graphModel());
    require(model,"typed editor model");const auto connections=model->allConnectionIds(11).size();
    scene->clearSelection();scene->nodeGraphicsObject(11)->setSelected(true);canvas->setFocus();QTest::keyClick(canvas,Qt::Key_Backspace);settle();
    require(!model->allNodeIds().contains(11),"delete shortcut");editor->history().undo();settle();require(model->allNodeIds().contains(11)&&model->allConnectionIds(11).size()==connections,"undo restores routes");
    require(presenter.client().snapshot().graph_revision==revision,"presentation changed processing");window.close();

}

static void chart_fill(GuiPresenter &presenter) {
    MainWindow window(presenter);emit presenter.active_graph_changed(presenter.client().snapshot().graph);
    window.resize(1200,850);window.show();settle();
    auto *fft=window.workspaceDock("fftDock"),*mel=window.workspaceDock("melDock"),*flow=window.workspaceDock("dataFlowDock");
    require(fft && mel && flow,"chart fill docks");
    const auto checkFill=[&] {
        settle();
        auto *panel=qobject_cast<GraphPanel*>(fft->widget());
        auto *splitter=panel?panel->findChild<QSplitter *>():nullptr;
        auto *chart=panel?panel->active_chart():nullptr;
        auto *stack=splitter?qobject_cast<QStackedWidget*>(splitter->widget(0)):nullptr;
        require(splitter && chart && stack && stack->currentWidget()==chart,"chart fill active renderer");
        qInfo()<<"chart fill"<<fft->size()<<panel->geometry()<<splitter->geometry()<<chart->geometry();
        require(panel->height()>=fft->height()-4,"chart panel does not fill dock");
        require(splitter->height()>=panel->height()-32,"chart/settings do not fill panel");
        require(stack->height()>=splitter->height()-4,"renderer stack does not fill splitter");
        require(QLineF(chart->geometry().topLeft(),stack->contentsRect().topLeft()).length()<=2 &&
                std::abs(chart->width()-stack->contentsRect().width())<=2 &&
                std::abs(chart->height()-stack->contentsRect().height())<=2,"active chart does not fill renderer stack");
        require(chart->rootObject() && chart->rootObject()->width()==chart->width() &&
                chart->rootObject()->height()==chart->height(),"QML plot root does not fill active renderer");
        chart->set_auto_resizing(false);chart->set_range(.0001,.0009);chart->set_axis_precision(0);
        const auto ticks=chart->presentation_model()->state().value("yTicks").toList();
        QSet<QString> labels;for(const auto& tick:ticks)labels.insert(tick.toMap().value("text").toString());
        require(labels.size()>1,"2D small-range ticks lose precision");
        require(chart->y_range().first==.0001 && chart->y_range().second==.0009,"formatter changes actual range");
    };
    fft->raise();checkFill();window.resize(1000,700);checkFill();
    mel->raise();settle();fft->raise();checkFill();
    flow->toggleView(false);checkFill();flow->toggleView(true);checkFill();
    const auto state=window.saveState(1);require(window.restoreState(state,1),"chart restore");fft->raise();checkFill();
    fft->setFloating(true);checkFill();window.tabifyDockWidget(mel,fft);fft->raise();checkFill();
    window.close();for(auto *dock:window.workspaceDocks())dock->close();
}

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    try {
        auto state = fixture();
        const auto mode = app.arguments().value(1);
        if(mode=="chrome") state.file_configs.ui.show_fps=true;
        require(qEnvironmentVariable("QT_QPA_PLATFORM")=="offscreen","isolated offscreen fixture only");
        GuiPresenter presenter(state,test_fixture::synthetic_engine_provider(false));presenter.initialize();
        require(gui_fixture::wait([&]{return presenter.client().snapshot().epoch!=0;}),"initial snapshot");
        presenter.apply_pipeline_graph(state.file_configs.scene_graph);
        require(gui_fixture::wait([&]{return presenter.applied_numeric_graph().has_value();}),"apply canonical fixture");
        if (mode == "chart") chart_fill(presenter);
        else if (mode == "selection") inspector_selection(presenter);
        else if (mode == "narrow") narrow_controls(presenter);
        else if (mode == "footer") dataflow_footer(presenter);
        else if (mode == "startup") restored_startup(presenter);
        else if (mode == "docking") graph_docking(presenter);
        else if (mode == "windows") scoped_windows(presenter);
        else if (mode == "chrome") compact_chrome(presenter);
        else throw std::runtime_error("Expected selection, narrow or footer case");
        qInfo("Qt workspace resize case passed: %s", qPrintable(mode));
        return 0;
    } catch (const std::exception &error) { qCritical("Qt workspace resize: %s", error.what()); return 1; }
}
