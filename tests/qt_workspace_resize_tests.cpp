#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/main_window.h"
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
    state.file_configs.scene_graph = pipeline::GraphProgram::defaults();
    state.file_configs.pipeline_graph = state.file_configs.scene_graph;
    state.delegate_model_registry = std::make_shared<QtNodes::NodeDelegateModelRegistry>();
    for (const auto &definition : pipeline::node_definitions())
        state.delegate_model_registry->registerModel([kind=definition.name] { return std::make_unique<AudioNodeModel>(kind); });
    state.data_flow_graph = std::make_shared<QtNodes::DataFlowGraphModel>(state.delegate_model_registry);
    state.data_flow_graph->load(state.file_configs.scene_graph);
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
    for (auto id : {QtNodes::NodeId(0), QtNodes::NodeId(5), QtNodes::NodeId(0)}) {
        scene->clearSelection(); scene->nodeGraphicsObject(id)->setSelected(true); emit scene->nodeSelected(id); settle();
        require(splitter->sizes() == widths, "Node selection changed workspace widths");
        require(QLineF(center, view->mapToScene(view->viewport()->rect().center())).length() < 2 / scale,
                "Node selection moved editor camera");
    }
    auto *handle = splitter->handle(2);
    QTest::mouseClick(handle, Qt::LeftButton); settle();
    require(splitter->sizes()[2] == 0, "Divider click did not collapse inspector");
    const auto collapsed = splitter->sizes();
    scene->clearSelection(); scene->nodeGraphicsObject(5)->setSelected(true); emit scene->nodeSelected(5); settle();
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
        require(title_bounds.right()<inspector->viewport()->width(),"Inspector title exceeds narrow viewport");
        auto *parameters = inspector->widget()->findChild<QWidget *>("pipelineNodeParameters");
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
    MainWindow window(presenter);
    // Finish initial engine snapshot/navigation delivery before the user closes
    // graph areas. Otherwise the bootstrap migration legitimately opens them.
    settle();
    auto *flow = window.workspaceDock("dataFlowDock");
    require(flow, "Data Flow dock absent");
    for (auto *dock : window.workspaceDocks()) if (dock != flow) dock->hide();
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
        MainWindow window(presenter);
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
    MainWindow window(presenter); window.resize(1000,700); window.show(); presenter.initialize(); settle();
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
    emit editor->graph_open_requested(7,0); settle();
    auto *surface=window.workspaceDock("spectrogramDock");
    require(surface->isFloating() && surface->floatingDockContainer(),"External graph lacks a separate ADS container");
    require(ads::CDockManager::testConfigFlag(ads::CDockManager::DragPreviewIsDynamic),"Docking does not use cancellable preview");
    surface->findChild<QAction *>("graphPlacement")->menu()->actions()[1]->trigger(); settle();
    require(!surface->isFloating(),"Native-framed graph cannot return to workspace");
    require(surface->features() & WorkspaceDock::DockWidgetMovable,"Returned graph cannot be rearranged internally");
    window.close(); surface->close();
}

static void scoped_windows(GuiPresenter &presenter) {
    MainWindow window(presenter);
    window.resize(1500,900); window.show(); presenter.initialize(); QTest::qWait(100);
    presenter.stop_analyzer(); presenter.stop_loopback(); QTest::qWait(60);
    auto *editor=window.findChild<SchemeEditorWidget *>();
    auto *flow=window.workspaceDock("dataFlowDock");
    auto *fft=window.workspaceDock("fftDock");
    auto *mel=window.workspaceDock("melDock");
    auto *surface=window.workspaceDock("spectrogramDock");
    for(auto *dock:window.workspaceDocks()) if(dock!=flow) dock->hide();
    emit editor->graph_view_requested(5); settle();
    require(fft->isHidden(),"Flow-only click created a graph");
    emit editor->graph_open_requested(7,0); settle();
    const auto externalGeometry=rectInWindow(surface);
    emit editor->graph_view_requested(5); settle();
    require(surface->isVisible() && surface->isFloating() && rectInWindow(surface)==externalGeometry && fft->isHidden(),
            "Flow click replaced an external graph");
    emit editor->graph_open_requested(5,2); settle();
    require(!fft->isFloating() && !fft->isHidden() && rectInWindow(fft).top()<rectInWindow(flow).top(),"First internal graph did not open above flow");
    emit editor->graph_open_requested(6,2); settle();
    require(fft->isVisible() && mel->isVisible() && !window.tabifiedDockWidgets(fft).contains(mel),
            "Two internal graph areas were not created");
    QFocusEvent externalFocus(QEvent::FocusIn); QApplication::sendEvent(surface->widget(),&externalFocus);
    emit editor->graph_view_requested(9); settle();
    require(surface->isVisible() && surface->isFloating() && !fft->isHidden(),"External focus redirected internal switching");
    emit editor->graph_open_requested(6,0); settle();
    require(mel->isFloating() && surface->isFloating(),"Two external windows did not remain independent");
    emit editor->graph_view_requested(7); settle();
    require(surface->isFloating() && surface->isVisible() && mel->isFloating() && mel->isVisible(),
            "Switching to an externally displayed graph moved its external window");
    int internalSurfaces=0;
    for(auto *dock:window.workspaceDocks())
        if(!dock->isFloating() && !dock->isHidden() && qobject_cast<SpectrogramGraph *>(dock->widget())) ++internalSurfaces;
    require(internalSurfaces==1,"Internal slot did not obtain its own graph representation");
    emit editor->graph_open_requested(5,2); settle();
    QList<WorkspaceDock *> visibleAreas;
    for(auto *dock:window.workspaceDocks())
        if(dock!=flow && !dock->isFloating() && dock->property("renderExposed").toBool()) visibleAreas.append(dock);
    require(visibleAreas.size()==3,"Repeated internal open did not create three areas");
    for(int i=0;i<visibleAreas.size();++i) for(int j=i+1;j<visibleAreas.size();++j)
        require(!rectInWindow(visibleAreas[i]).intersects(rectInWindow(visibleAreas[j])),"Three internal areas overlap");
    auto *scene=editor->findChild<QtNodes::DataFlowGraphicsScene *>();
    auto *menu=scene->createSceneMenu(scene->nodeGraphicsObject(7)->sceneBoundingRect().center());
    auto *representations=menu->findChild<QMenu *>("graphRepresentations");
    require(representations && representations->actions().size()==2,"Context menu does not list internal and external representations");
    delete menu;
    auto *position=window.findChild<QComboBox *>("dataFlowPosition");
    require(position,"Workspace position selector absent");
    for(int value:{1,2,3,0}) {
        position->setCurrentIndex(value); settle();
        for(auto *area:visibleAreas) {
            if(rectInWindow(area).intersects(rectInWindow(flow))) qWarning()<<"Position"<<value<<"flow"<<rectInWindow(flow)<<area->objectName()<<rectInWindow(area);
            require(!rectInWindow(area).intersects(rectInWindow(flow)),"Positioned flow overlaps a graph");
            if(value==0) require(rectInWindow(area).bottom()<rectInWindow(flow).top(),"Flow is not below graphs");
            if(value==1) require(rectInWindow(flow).bottom()<rectInWindow(area).top(),"Flow is not above graphs");
            if(value==2) require(rectInWindow(flow).right()<rectInWindow(area).left(),"Flow is not left of graphs");
            if(value==3) require(rectInWindow(area).right()<rectInWindow(flow).left(),"Flow is not right of graphs");
        }
    }
    pipeline::TypedFrame frame; frame.type=pipeline::FrameType::Spectrum; frame.values={.2f,.3f};
    emit presenter.node_views_ready({{5,frame}}); settle();
    int updatedFftViews=0;
    for(auto *dock:visibleAreas) if(dock->property("graphNode").toInt()==5) {
        auto *chart=dynamic_cast<AbstractChartView *>(dock->findChild<QChartView *>());
        if(chart && chart->value_count()!=2) qWarning()<<"FFT frame"<<dock->objectName()<<chart->value_count()<<dock->property("renderExposed");
        require(chart && chart->value_count()==2,"An independent FFT representation did not receive frames");
        ++updatedFftViews;
    }
    require(updatedFftViews==2,"Repeated graph open did not retain independent renderers");
    window.close();
    for(auto *dock:window.workspaceDocks()) dock->close();
    auto restoredState=fixture(); restoredState.file_configs=presenter.state().file_configs;
    GuiPresenter restoredPresenter(restoredState,test_fixture::synthetic_engine_provider());
    MainWindow restored(restoredPresenter); restored.show(); restoredPresenter.initialize(); QTest::qWait(80);
    int externalCount=0,internalCount=0;
    for(auto *dock:restored.workspaceDocks()) if(!dock->isHidden() && dock->property("graphNode").isValid()) {
        if(dock->isFloating()) ++externalCount;
        else if(dock->property("renderExposed").toBool()) ++internalCount;
    }
    require(externalCount==2 && internalCount==3,"Restore lost independent windows or internal areas");
    restored.close();
    for(auto *dock:restored.workspaceDocks()) dock->close();
}

static void graph_docking(GuiPresenter &presenter) {
    MainWindow window(presenter);
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
#ifdef Q_OS_WIN
    // Qt Windows strips device identity and reads keyboard state instead of
    // MK_CONTROL in the wheel message. Replay that platform boundary.
    auto nativeWheel=[&](UINT message, short delta, WORD keys, DWORD time) {
        MSG native{}; native.message=message; native.time=time;
        native.wParam=MAKEWPARAM(keys,WORD(delta));
        qintptr result=0;
        QAbstractEventDispatcher::instance()->filterNativeEvent("windows_generic_MSG",&native,&result);
        QWheelEvent wheel(pointer,canvas->viewport()->mapToGlobal(pointer.toPoint()),{},
                          message==WM_MOUSEHWHEEL ? QPoint(-delta,0) : QPoint(0,delta),
                          Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
        wheel.setTimestamp(time);
        QApplication::sendEvent(canvas->viewport(),&wheel);
    };
    const auto nativeTransform=canvas->transform();
    const auto nativeCenter=canvas->mapToScene(canvas->viewport()->rect().center());
    nativeWheel(WM_MOUSEWHEEL,-40,0,101);
    nativeWheel(WM_MOUSEHWHEEL,40,0,102);
    const auto nativeMoved=canvas->mapToScene(canvas->viewport()->rect().center());
    require(canvas->transform()==nativeTransform && nativeMoved.y()>nativeCenter.y()+5 && nativeMoved.x()>nativeCenter.x()+5,
            "Windows unlabelled touchpad swipe does not pan both axes");
    nativeWheel(WM_MOUSEWHEEL,40,MK_CONTROL,103);
    require(canvas->transform()!=nativeTransform,"Windows pinch lost native MK_CONTROL and panned");
#endif
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
    emit editor->graph_open_requested(5,0); settle();
    require(fft->isFloating() && fft->isVisible() && fft->window()!=&window,"Separate-window command remained docked");
    fft->findChild<QAction *>("graphPlacement")->menu()->actions()[1]->trigger(); settle();
    require(!fft->isFloating() && !fft->isHidden(),"Return to tab failed");
    const auto flowBefore=rectInWindow(flow);
    emit editor->graph_open_requested(6,2); QTest::qWait(100);
    require(!mel->isFloating() && !window.tabifiedDockWidgets(fft).contains(mel),"Beside command created a tab");
    require(rectInWindow(fft).right()<rectInWindow(mel).left() || rectInWindow(mel).right()<rectInWindow(fft).left(),
            "Side-by-side graph areas overlap");
    require(std::abs(rectInWindow(flow).top()-flowBefore.top())<15,"Splitting graphs relocated Data Flow");
    require(rectInWindow(flow).bottom()<(window.statusBar()->isVisible() ? window.statusBar()->geometry().top() : window.height()),"Dock separator overlays footer");
    require(rectInWindow(fft).width()>150 && rectInWindow(mel).width()>150,"Split left an unusable graph area");
    const auto priorWidth=mel->width();
    auto *splitter=qobject_cast<QSplitter *>(fft->dockAreaWidget()->parentWidget());
    require(splitter && splitter->orientation()==Qt::Horizontal,"Adjacent graph splitter absent");
    auto *handle=splitter->handle(splitter->indexOf(mel->dockAreaWidget()));
    require(handle && handle->isVisible(),"Graph separator is not visible");
    const auto center=handle->rect().center();
    QTest::mousePress(handle,Qt::LeftButton,Qt::NoModifier,center);
    QMouseEvent move(QEvent::MouseMove,center+QPoint(40,0),handle->mapToGlobal(center+QPoint(40,0)),Qt::NoButton,Qt::LeftButton,Qt::NoModifier);
    QApplication::sendEvent(handle,&move); settle();
    QTest::mouseRelease(handle,Qt::LeftButton,Qt::NoModifier,center+QPoint(40,0)); settle();
    require(mel->width()!=priorWidth,"Graph divider cannot resize adjacent areas");
    emit editor->graph_open_requested(6,3); QTest::qWait(100);
    require(rectInWindow(fft).bottom()<rectInWindow(mel).top() || rectInWindow(mel).bottom()<rectInWindow(fft).top(),
            "Below command did not separate graph areas vertically");
    require(rectInWindow(flow).bottom()<(window.statusBar()->isVisible() ? window.statusBar()->geometry().top() : window.height()),"Vertical split overlaps footer");
    emit editor->graph_open_requested(6,0); settle();
    require(mel->isFloating(),"Split graph cannot detach");
    auto *scene=editor->findChild<QtNodes::DataFlowGraphicsScene *>();
    auto *view=editor->findChild<QGraphicsView *>();
    require(scene && view,"Graph shortcut canvas absent");
    scene->clearSelection(); scene->nodeGraphicsObject(5)->setSelected(true);
    window.activateWindow(); view->setFocus(); settle();
    QTest::keyClick(view,Qt::Key_G,Qt::AltModifier|Qt::ShiftModifier); settle();
    require(fft->isFloating(),"Alt+Shift+G did not open selected graph separately");
    window.activateWindow(); view->setFocus(); settle();
    QTest::keyClick(view,Qt::Key_G,Qt::AltModifier); settle();
    bool internalFft=false;
    for(auto *dock:window.workspaceDocks())
        if(dock->property("graphNode").toInt()==5 && !dock->isFloating() && !dock->isHidden()) internalFft=true;
    require(internalFft && fft->isFloating(),"Alt+G moved the external window instead of opening an internal view");
    auto *surface=window.workspaceDock("spectrogramDock");
    auto *surfaceContent=surface->widget();
    QElapsedTimer pinTime; pinTime.start();
    for(int i=0;i<3;++i) {
        surface->setAutoHide(true,ads::SideBarRight); settle();
        require(surface->isAutoHide(),"Pin did not collapse the graph into its sidebar");
        surface->setAutoHide(false); surface->raise(); settle();
        require(surface->widget()==surfaceContent && !surface->isAutoHide(),"Pin recreated the renderer or failed to restore");
    }
    qInfo("Graph pin/restore: three cycles completed in %lld ms",pinTime.elapsed());
    auto &graph=static_cast<QtNodes::DataFlowGraphModel &>(scene->graphModel());
    const auto connectionCount=graph.save().value("connections").toArray().size();
    scene->clearSelection(); scene->nodeGraphicsObject(6)->setSelected(true);
    window.activateWindow(); view->setFocus(); settle();
    QTest::keyClick(view,Qt::Key_Backspace); settle();
    require(!graph.allNodeIds().contains(6),"Backspace did not delete selected node");
    auto *undo=window.findChild<QAction *>("undoDataFlow");
    auto *redo=window.findChild<QAction *>("redoDataFlow");
    require(undo && redo && undo->isEnabled() && window.findChild<QMenu *>("fileMenu")->actions().contains(undo),"File history actions missing");
    undo->trigger(); settle();
    require(graph.allNodeIds().contains(6),"Node deletion lost undo support");
    require(graph.save().value("connections").toArray().size()==connectionCount,"Undo did not restore deleted node connections");
    require(redo->isEnabled(),"File redo unavailable after undo");
    redo->trigger(); settle(); require(!graph.allNodeIds().contains(6),"File redo did not repeat deletion");
    undo->trigger(); settle();
    window.close(); mel->close();
}

int main(int argc, char **argv) {
    QCoreApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    try {
        auto state = fixture();
        const auto mode = app.arguments().value(1);
        if (mode == "startup") {
            QFile saved(app.arguments().value(2));
            require(saved.open(QIODevice::ReadOnly), "Saved dock fixture unavailable");
            state.file_configs.ui.workspace = QJsonDocument::fromJson(saved.readAll()).object();
            state.file_configs.ui.graph_views["5b226c6f63616c222c226d61696e222c31335d"] =
                QJsonObject{{"node", 13}, {"kind", "FFTView"}, {"device", "local"}, {"graph", "main"}, {"open", true}};
        }
        if(mode=="chrome") state.file_configs.ui.show_fps=true;
        GuiPresenter presenter(state, test_fixture::synthetic_engine_provider());
        if (mode == "selection") inspector_selection(presenter);
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
