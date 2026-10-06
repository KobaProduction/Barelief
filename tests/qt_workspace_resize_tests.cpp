#include "synthetic_engine_provider.h"
#include "core_app/gui_presenter.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/audio_node_model.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/ui_text.h"
#include <QtNodes/internal/NodeGraphicsObject.hpp>
#include <QApplication>
#include <QComboBox>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QLabel>
#include <QMenuBar>
#include <QScrollArea>
#include <QSplitter>
#include <QSplitterHandle>
#include <QStatusBar>
#include <QFile>
#include <QJsonDocument>
#include <QtTest/QTest>
#include <stdexcept>

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
    auto *flow = window.findChild<QDockWidget *>("dataFlowDock");
    require(flow, "Data Flow dock absent");
    for (auto *dock : window.findChildren<QDockWidget *>()) if (dock != flow) dock->hide();
    window.resize(800, 600); window.show(); settle();
    for (const auto size : {QSize(800,600), QSize(1200,760), QSize(800,600)}) {
        window.resize(size); settle();
        require(flow->geometry().bottom() < window.statusBar()->geometry().top(), "Data Flow intersects footer");
        require(flow->geometry().left() <= 1 && flow->geometry().right() >= window.width()-2,
                "Only Data Flow retained an empty dock splitter area");
        if (flow->geometry().top() > window.menuBar()->geometry().bottom()+2)
        {
            qWarning() << "flow" << flow->geometry() << "menu" << window.menuBar()->geometry() << "footer" << window.statusBar()->geometry();
            for (auto *dock : window.findChildren<QDockWidget *>()) qWarning() << dock->objectName() << dock->isHidden() << dock->geometry();
        }
        require(flow->geometry().top() <= window.menuBar()->geometry().bottom()+2,
                "Only Data Flow retained an empty upper dock area");
        for (auto *control : {window.findChild<QWidget *>("pipelineToggle"), window.findChild<QWidget *>("themeMenuButton")}) {
            require(control, "Footer control absent");
            const QRect bounds(control->mapTo(&window,QPoint()),control->size());
            require(window.statusBar()->geometry().contains(bounds), "Footer control escapes footer");
        }
    }
    window.close();
}

static void restored_startup(GuiPresenter &presenter) {
    // Restore a saved tabbed workspace before the first event loop, as the
    // desktop entrypoint does, then close and restore its resulting layout.
    for (int pass = 0; pass < 4; ++pass) {
        MainWindow window(presenter);
        auto *fft = window.findChild<QDockWidget *>("fftDock");
        auto *mel = window.findChild<QDockWidget *>("melDock");
        require(fft && mel, "Saved workspace graph docks absent");
        if (pass == 0) presenter.initialize();
        window.show();
        QTest::qWait(250); // Includes deferred dock animation deletion.
        require(window.isVisible(), "Restored workspace did not remain open");
        window.close();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }
}

int main(int argc, char **argv) {
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
        GuiPresenter presenter(state, test_fixture::synthetic_engine_provider());
        if (mode == "selection") inspector_selection(presenter);
        else if (mode == "narrow") narrow_controls(presenter);
        else if (mode == "footer") dataflow_footer(presenter);
        else if (mode == "startup") restored_startup(presenter);
        else throw std::runtime_error("Expected selection, narrow or footer case");
        qInfo("Qt workspace resize case passed: %s", qPrintable(mode));
        return 0;
    } catch (const std::exception &error) { qCritical("Qt workspace resize: %s", error.what()); return 1; }
}
