#include "synthetic_capture.h"
#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/scheme_editor/editor_widget.h"
#include "visualization/charts/freq.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <DockAreaWidget.h>
#include <QToolBar>
#include <FloatingDockContainer.h>
#include <QTest>
#include <QProcess>
#include <QOpenGLWidget>
#include <QToolButton>
#include <QMenu>
#include <QMenuBar>
#include <QAction>
#include <QStatusBar>
#include <QGraphicsView>
#include <QFocusEvent>
#include "core_app/settings_widget.h"
#include "visualization/ui_text.h"
#include <stdexcept>
static QRect rectInWindow(QWidget *widget) {
    if(auto *dock=qobject_cast<WorkspaceDock *>(widget); dock && dock->dockAreaWidget()) widget=dock->dockAreaWidget();
    return QRect(widget->mapTo(widget->window(),QPoint()),widget->size());
}

static void require(bool value, const char *message) {
    if (!value)
        throw std::runtime_error(message);
}
int main(int argc, char **argv) {
    try {
        QTemporaryDir dir;
        const bool restore = argc == 3;
        const auto path = restore ? QString::fromLocal8Bit(argv[2]) : dir.filePath("views.json");
        auto graph = pipeline::GraphProgram::test_signal_defaults();
        auto nodes = graph["nodes"].toArray();
        nodes.append(
            QJsonObject{{"id", 55},
                        {"internal-data", QJsonObject{{"model-name", "FFTView"}, {"channel", 1}, {"merge", 2}}},
                        {"position", QJsonObject{{"x", 1500}, {"y", 400}}}});
        graph["nodes"] = nodes;
        auto edges = graph["connections"].toArray();
        edges.append(QJsonObject{{"outNodeId", 1}, {"outPortIndex", 0}, {"inNodeId", 55}, {"inPortIndex", 0}});
        graph["connections"] = edges;
        if (!restore) {
            QFile file(path);
            require(file.open(QIODevice::WriteOnly), "fixture");
            file.write(
                QJsonDocument(QJsonObject{{"core", QJsonObject{{"run_loopback", false}, {"run_analyser", false}}},
                                          {"pipeline_graph", graph},
                                          {"scene_graph", graph}})
                    .toJson());
        }
        auto bytes = path.toLocal8Bit();
        char name[] = "gui_views_tests", flag[] = "--config";
        char *args[]{name, flag, bytes.data(), nullptr};
        int count = 3;
        CoreApplication app(count, args, test_fixture::synthetic_capture_factory(false));
        MainWindow *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(widget))
                window = candidate;
        require(window, "window");
        window->show();
        QTest::qWait(250);
        const auto extra_key =
            QString::fromLatin1(QJsonDocument(QJsonArray{app.presenter().local_device_id(), "main", 55})
                                    .toJson(QJsonDocument::Compact)
                                    .toHex());
        auto *extra = window->workspaceDock("nodeView_" + extra_key);
        require(extra, "second FFT dock not created");
        require(window->workspaceDocks().size() >= 9, "unexpected duplicate or missing views");
        auto *first = window->workspaceDock("fftDock");
        if(argc==2 && QString::fromLocal8Bit(argv[1])=="--navigation-only") {
            auto *surface=window->workspaceDock("spectrogramDock");
            auto *editor=window->findChild<SchemeEditorWidget *>();
            require(surface && editor && !first->isHidden() && !extra->isHidden(),"Active graph tabs absent");
            require(window->tabifiedDockWidgets(surface).contains(first) &&
                    window->tabifiedDockWidgets(surface).contains(extra),"Graph nodes were not grouped in tabs");
            const auto revision=app.pipeline().graph_revision();
            auto *canvas=editor->findChild<QGraphicsView *>();
            canvas->setFocus();app.processEvents();
            emit editor->graph_view_requested(55);app.processEvents();
            require(extra->widget()->isVisible(),"Graph block activation did not focus its tab");
            require(canvas->hasFocus(),"Graph activation stole editor shortcut focus");
            first->hide();emit editor->graph_view_requested(5);app.processEvents();
            require(!first->isHidden() && first->widget()->isVisible(),"Graph block did not reopen/focus its hidden view");
            require(!first->findChild<QToolButton *>("graphSelector") &&
                    !first->findChild<QToolButton *>("toggleFloatingGraph"),"Duplicate graph controls retained");
            auto *placement = extra->findChild<QAction *>("graphPlacement");
            auto *renderer = dynamic_cast<FreqChartView *>(extra->findChild<QChartView *>());
            require(placement && renderer && placement->menu()->actions().size()==4,"Explicit placement actions absent");
            placement->menu()->actions()[0]->trigger(); app.processEvents();
            require(extra->isFloating() && extra->property("renderExposed").toBool(),"Separate-window action failed");
            placement->menu()->actions()[1]->trigger(); app.processEvents();
            require(!extra->isFloating() && extra->findChild<QChartView *>()==renderer,"Return to tabs recreated view");
            emit editor->graph_view_requested(5); app.processEvents();
            require(first->property("renderExposed").toBool() && !extra->property("renderExposed").toBool(),
                    "Inactive graph tab remains exposed for rendering");
            pipeline::TypedFrame frame; frame.type=pipeline::FrameType::Spectrum; frame.values={1,2,3};
            const auto hidden_count=renderer->value_count();
            emit app.presenter().node_views_ready({{55,frame}});
            require(renderer->value_count()==hidden_count,"Hidden graph still receives render data");
            placement->menu()->actions()[2]->trigger(); app.processEvents();
            require(first->property("renderExposed").toBool() && extra->property("renderExposed").toBool(),
                    "Side-by-side graphs did not both become exposed");
            emit app.presenter().node_views_ready({{55,frame}});
            require(renderer->value_count()==3,"Exposed graph does not resume data updates");
            placement->menu()->actions()[1]->trigger(); app.processEvents();
            auto *menu=window->findChild<QMenu *>("graphsMenu");
            require(menu && menu->title()==ui::text("Display") && !menu->actions().contains(first->toggleViewAction()),
                    "Top Display menu retained graph list");
            auto *settings=window->findChild<SettingsWidget *>();
            require(settings && settings->findChild<QGroupBox *>("graphViewsSettings"),"Graph list was not moved into settings");
            require(window->findChild<QAction *>("saveSettings")->shortcut()==QKeySequence::Save,"Ctrl+S was not restored");
            require(window->findChild<QAction *>("saveSettingsAs")->shortcut()==QKeySequence::SaveAs &&
                    window->findChild<QAction *>("openSettingsFile")->shortcut()==QKeySequence::Open,
                    "Standard file shortcuts absent");
            emit editor->graph_open_requested(55,0);app.processEvents();
            require(extra->isFloating(),"Context graph window request failed");
            placement->menu()->actions()[1]->trigger();app.processEvents();
            require(!window->findChild<QMenu *>("pipelinePresetsMenu")->title().isEmpty(),"Display presets submenu has no title");
            for(auto *dock:window->workspaceDocks()) if(dock->objectName()!="dataFlowDock") dock->hide();
            app.processEvents();
            emit editor->graph_view_requested(5);app.processEvents();
            require(first->isHidden(),"Ordinary graph click created an unreserved area");
            auto *flow=window->workspaceDock("dataFlowDock");
            require(flow && rectInWindow(flow).bottom()<(window->statusBar()->isVisible() ? window->statusBar()->geometry().top() : window->height()),
                    "Single Data Flow overlaps footer");
            emit editor->graph_open_requested(5,1);app.processEvents();
            require(!first->isHidden(),"Explicit context command did not reserve a graph area");
            const auto region=rectInWindow(first);
            emit editor->graph_view_requested(55);app.processEvents();
            require(first->isHidden() && !extra->isHidden() && (rectInWindow(extra).center()-region.center()).manhattanLength()<15,
                    "Switching a graph did not replace the reserved area");
            emit editor->graph_open_requested(5,2);app.processEvents();
            const auto untouched=rectInWindow(first), replaced=rectInWindow(extra);
            QFocusEvent focusIn(QEvent::FocusIn);QApplication::sendEvent(extra->widget(),&focusIn);
            emit editor->graph_view_requested(6);app.processEvents();
            auto *mel=window->workspaceDock("melDock");
            require(!first->isHidden() && extra->isHidden() && !mel->isHidden() &&
                    (rectInWindow(first).center()-untouched.center()).manhattanLength()<15 &&
                    (rectInWindow(mel).center()-replaced.center()).manhattanLength()<15,
                    "Graph switch ignored the last interacted viewing area");
            require(window->menuBar()->isAncestorOf(window->findChild<QToolButton *>("pipelinePresets")),
                    "Presets not moved to the upper bar");
            auto *fileMenu=window->findChild<QMenu *>("fileMenu");
            require(fileMenu && fileMenu->actions().contains(window->findChild<QAction *>("saveSettings")) &&
                    window->findChild<QAction *>("saveSettingsAs") && window->findChild<QAction *>("openSettingsFile"),
                    "File settings actions absent");
            const auto savedPath=dir.filePath("saved-as.json");
            emit window->save_configuration_as_requested(savedPath);
            QFile saved(savedPath);require(saved.open(QIODevice::ReadOnly) &&
                QJsonDocument::fromJson(saved.readAll()).isObject(),"Save settings as did not write JSON");
            saved.close();
            QFile invalid(dir.filePath("invalid.json"));require(invalid.open(QIODevice::WriteOnly),"Invalid fixture");
            invalid.write("{ invalid json");invalid.close();
            emit window->open_configuration_requested(invalid.fileName());
            require(window->isVisible() && !window->statusBar()->currentMessage().isEmpty(),
                    "Invalid settings open did not retain window and report error");
            require(window->findChild<QAction *>("resetCameraView")->shortcut()==QKeySequence("Alt+R"),"Reset camera shortcut not discoverable");
            require(app.pipeline().graph_revision()==revision,"Graph navigation changed DSP revision");
            window->close();qInfo("Graph tabs, activation, selector, settings and shortcut navigation passed");return 0;
        }
        auto *editor = window->findChild<SchemeEditorWidget *>();
        require(editor, "graph editor absent");
        first->show();
        first->raise();
        extra->show();
        // Public QDock floating also represents dragging a tab into its own
        // window; exposure must update without a placement-menu command.
        extra->setFloating(true);
        for (int i=0; i<100 && (!first->property("renderExposed").toBool() || !extra->property("renderExposed").toBool()); ++i)
            QTest::qWait(5);
        require(first->property("renderExposed").toBool() && extra->property("renderExposed").toBool(),
                "Floating an inactive tab did not expose both renderers");
        auto *a = dynamic_cast<FreqChartView *>(first->findChild<QChartView *>());
        auto *b = dynamic_cast<FreqChartView *>(extra->findChild<QChartView *>());
        require(a && b && a != b, "independent charts absent");
        if (restore) {
            require(extra->isFloating(), "dynamic dock layout not restored");
            window->close();
            return 0;
        }
        pipeline::TypedFrame left, right;
        left.type = right.type = pipeline::FrameType::Spectrum;
        left.values = {1, 2, 3};
        right.values = {8, 9};
        left.frequency_step = right.frequency_step = 10;
        emit app.presenter().node_views_ready({{5, left}, {55, right}});
        if (a->value_count() != 3 || b->value_count() != 2 || a->peak_value() != 3 || b->peak_value() != 9)
            qCritical() << "Per-node values/counts:" << a->value_count() << a->peak_value() << b->value_count() << b->peak_value()
                        << "Visibility:" << first->isVisible() << extra->isVisible();
        require(a->value_count() == 3 && b->value_count() == 2 && a->peak_value() == 3 && b->peak_value() == 9,
                "per-node frames cross-fed views");
        emit app.presenter().node_views_ready({{55, right}});
        require(a->value_count() == 3 && a->peak_value() == 0 && b->value_count() == 2 && b->peak_value() == 9,
                "Missing node frame did not zero only its existing geometry");
        pipeline::TypedFrame unavailable;
        unavailable.available = false;
        unavailable.sequence = 0;
        emit app.presenter().node_views_ready({{5, left}, {55, unavailable}});
        require(a->value_count() == 3 && a->peak_value() == 3 && b->value_count() == 2 && b->peak_value() == 0,
                "Unavailable marker did not zero only affected node geometry");
        if (app.platformName() != "offscreen" && app.platformName() != "minimal") {
            auto *dock = window->workspaceDock("spectrogramDock");
            require(dock, "spectrogram dock");
            dock->show();
            dock->raise();
            QTest::qWait(100);
            auto *surface = dock->findChild<SpectrogramGraph *>();
            auto *native = surface->findChild<QOpenGLWidget *>("spectrogramContainer");
            require(native && native->isValid(), "native surface");
            pipeline::TypedFrame history;
            history.type = pipeline::FrameType::History;
            history.history_capacity = 7;
            history.bands = 3;
            history.history.assign(10, std::vector<float>(3, .2f));
            history.history[9][1] = 1.8f;
            const auto before = native->property("geometryUploads").toULongLong();
            emit app.presenter().node_views_ready({{5, left}, {55, right}, {7, history}});
            for (int wait = 0; wait < 50 && native->property("geometryUploads").toULongLong() == before; ++wait)
                QTest::qWait(10);
            require(native->property("historyCapacity").toULongLong() == 7 &&
                        native->property("historyBands").toULongLong() == 3 &&
                        native->property("validHistoryColumns").toULongLong() == 7,
                    "Frame history metadata was transposed/clipped");
            require(native->property("surfacePeakFrame").toULongLong() == 6 &&
                        native->property("surfacePeakBand").toULongLong() == 1,
                    "Frame-major history peak crossed time/band axes");
            qInfo("GUI typed history: frame-major10x3,capacity7,peakframe6/band1 PASS");
        }
        emit app.presenter().views_cleared();
        require(a->value_count() == 0 && b->value_count() == 0, "disconnect retained frame");
        auto preferences = app._ctx.file_configs.ui.graph_views;
        bool identity = false;
        for (auto it = preferences.begin(); it != preferences.end(); ++it) {
            auto p = it.value().toObject();
            if (p["node"].toInt() == 55)
                identity = p["device"].toString() == app.presenter().device_id() &&
                           p["graph"].toString() == app.presenter().graph_id();
        }
        require(identity, "identity not persisted");
        {
            pipeline::AudioPipeline remote_engine(test_fixture::synthetic_capture_factory());
            auto remote_graph = graph;
            auto remote_nodes = remote_graph["nodes"].toArray();
            remote_nodes.append(QJsonObject{
                {"id", 100},
                {"internal-data",
                 QJsonObject{{"model-name", "NetworkInput"}, {"data_type", "mel"}, {"stream_id", "gui-control"}}}});
            remote_nodes.append(QJsonObject{{"id", 101}, {"internal-data", QJsonObject{{"model-name", "MelView"}}}});
            remote_graph["nodes"] = remote_nodes;
            auto remote_edges = remote_graph["connections"].toArray();
            remote_edges.append(
                QJsonObject{{"outNodeId", 100}, {"outPortIndex", 0}, {"inNodeId", 101}, {"inPortIndex", 0}});
            remote_graph["connections"] = remote_edges;
            QString error;
            if (!remote_engine.apply_graph(remote_graph, error)) {
                qCritical() << error;
                return 1;
            }
            pipeline::EngineServer server(remote_engine);
            require(server.listen(QHostAddress::LocalHost, 0, false), "remote server");
            app.presenter().select_device("remote-test", "127.0.0.1", server.port());
            auto wait = [&](auto predicate, const char *message) {
                QElapsedTimer clock;
                clock.start();
                while (!predicate() && clock.elapsed() < 4000)
                    QTest::qWait(5);
                if (!predicate())
                    qCritical() << message << "Remote status:" << int(remote_engine.current_status("Capture").state)
                                << int(remote_engine.current_status("Analyzer").state)
                                << "processed:" << remote_engine.processed_frames()
                                << "epoch:" << app.presenter().client().snapshot().epoch
                                << "GUI status:" << int(app.presenter().client().snapshot().statuses.value("Capture").state)
                                << int(app.presenter().client().snapshot().statuses.value("Analyzer").state);
                require(predicate(), message);
            };
            wait([&] { return app.presenter().client().snapshot().epoch != 0; }, "Remote control handshake timeout");
            auto dock_for = [&](int node) {
                auto key = QString::fromLatin1(
                    QJsonDocument(QJsonArray{"remote-test", "main", node}).toJson(QJsonDocument::Compact).toHex());
                return window->workspaceDock("nodeView_" + key);
            };
            auto *fft_remote = dock_for(5), *mel_remote = dock_for(101);
            require(fft_remote && mel_remote, "remote per-node views absent");
            emit editor->graph_open_requested(5, 0);
            emit editor->graph_open_requested(101, 0);
            wait([&] { return fft_remote->property("renderExposed").toBool() && mel_remote->property("renderExposed").toBool(); },
                 "Remote floating renderer exposure timeout");
            auto *fft_chart = dynamic_cast<AbstractChartView *>(fft_remote->findChild<QChartView *>());
            auto *mel_chart = dynamic_cast<AbstractChartView *>(mel_remote->findChild<QChartView *>());
            require(fft_chart && mel_chart, "remote chart types");
            remote_engine.start_analyzer();
            remote_engine.start_capture();
            wait([&] { return fft_chart->value_count() > 0; }, "Remote synthetic FFT delivery timeout");
            pipeline::TypedFrame incoming;
            incoming.type = pipeline::FrameType::Mel;
            incoming.values = {42, 43};
            incoming.sequence = 1;
            incoming.generation = 99;
            incoming.sample_rate = 48000;
            incoming.stream_id = "gui-control";
            remote_engine.feed_typed(100, incoming);
            wait([&] { return mel_chart->value_count() == 2; }, "Remote injected Mel delivery timeout");
            remote_engine.clear_ingress(100, remote_engine.epoch());
            wait([&] { return mel_chart->value_count() == 2 && mel_chart->peak_value() == 0; }, "Remote unavailable Mel zero timeout");
            require(fft_chart->value_count() > 0, "Upstream unavailable marker cleared independent remote branch");
            incoming.sequence = 2;
            remote_engine.feed_typed(100, incoming);
            wait([&] { return mel_chart->value_count() == 2 && mel_chart->peak_value() == 43; }, "Remote resumed Mel delivery timeout");
            remote_engine.stop_capture();
            remote_engine.stop_analyzer();
            for(int i=0; i<100 && app.presenter().client().snapshot().statuses.value("Analyzer").state!=pipeline::State::Stopped; ++i)
                QTest::qWait(5);
            if(mel_chart->value_count() || fft_chart->value_count())
                qInfo() << "External Stop before clear:" << "FFT" << fft_chart->value_count() << fft_chart->peak_value()
                        << "Mel" << mel_chart->value_count() << mel_chart->peak_value();
            wait([&] { return mel_chart->value_count() == 0 && fft_chart->value_count() == 0; }, "Remote explicit Stop clear timeout");
            wait([&] { return app.presenter().client().snapshot().statuses.value("Analyzer").state==pipeline::State::Stopped; },
                 "Remote explicit Stop acknowledgement timeout");
            app.presenter().select_device(app.presenter().local_device_id(), {}, 0);
        }
        window->close();
        QProcess child;
        child.start(QCoreApplication::applicationFilePath(), {"--restore", path});
        require(child.waitForStarted(3000) && child.waitForFinished(10000) && child.exitCode() == 0,
                "dynamic dock restart failed");
        qInfo("GUI views: independent per-node data, inactive/disconnected clear, identity and floating dock restart "
              "passed");
        return 0;
    } catch (const std::exception &error) {
        qCritical("%s", error.what());
        return 1;
    }
}
