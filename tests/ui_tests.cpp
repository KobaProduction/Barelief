#include "visualization/ui_text.h"
#include <QMenu>
#include <QMenuBar>
#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/settings_widget.h"
#include <QtTest/QTest>
#include <QCheckBox>
#include <QSpinBox>
#include <QTabWidget>
#include "dock_panels.h"
#include <QTemporaryDir>
#include <QFile>
#include <QScrollArea>
#include <Q3DCamera>
#include <Q3DSurface>
#include <QSplitter>
#include <QWheelEvent>
#include <QPushButton>
#include <QTimer>
#include <QDialog>
#include <QLabel>
#include "core_app/scheme_editor/audio_node_model.h"
#include <QToolBar>
#include <QAction>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QEventLoop>
#include "performance_report.h"
#include <stdexcept>
#include <cmath>
#include "visualization/charts/line.h"

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}
// QtNodes stores nodes/connections in unordered containers; load/save may reorder their arrays.
static QJsonObject canonical_editor_graph(QJsonObject graph) {
    for (const auto *key : {"nodes", "connections"}) {
        std::vector<QJsonObject> objects;
        for (const auto value : graph.value(key).toArray())
            objects.push_back(value.toObject());
        std::sort(objects.begin(), objects.end(), [](const auto &left, const auto &right) {
            return QJsonDocument(left).toJson(QJsonDocument::Compact) <
                   QJsonDocument(right).toJson(QJsonDocument::Compact);
        });
        QJsonArray sorted;
        for (const auto &object : objects)
            sorted.append(object);
        graph[key] = sorted;
    }
    return graph;
}
int main() {
    QTemporaryDir temp;
    auto path = temp.filePath("settings.json").toLocal8Bit();
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(
        R"({"core":{"run_loopback":false,"run_analyser":false},"ui":{"show_spectrogram":false},"wled_ar_server":{"port":80}})");
    file.close();
    int argc = 3;
    char name[] = "ui_tests", flag[] = "--config";
    char *argv[] = {name, flag, path.data(), nullptr};
    CoreApplication app(argc, argv);
    try {
        {
            LineChartView chart;
            chart.resize(480, 260);
            chart.update({0.0f, 1.0f, -1.0f, 0.5f});
            chart.show(); QTest::qWait(40);
            for (int i = 0; i < 12; ++i) chart.resize(490 + i * 2, 270 + i);
            QTest::qWait(40);
            require(chart.chart()->size() == QSizeF(chart.size()),
                    "Chart did not converge to the last visible resize");
            chart.hide();
            const auto hidden_size = chart.chart()->size();
            chart.resize(650, 340); QTest::qWait(40);
            require(chart.chart()->size() == hidden_size,
                    "Hidden chart performed resize layout work");
            chart.show(); QTest::qWait(40);
            require(chart.chart()->size() == QSizeF(chart.size()),
                    "Reopened chart retained stale layout dimensions");
        }
        MainWindow *window = nullptr;
        for (auto *widget : QApplication::topLevelWidgets())
            if (auto *candidate = qobject_cast<MainWindow *>(widget))
                window = candidate;
        require(window, "Main window not created");
        auto *settings = window->findChild<SettingsWidget *>();
        DockPanels panel_views(window); auto *tabs = &panel_views;
        require(settings && tabs, "Settings tab not created");
        auto *settings_dialog = window->findChild<QDialog *>("settingsDialog");
        window->findChild<QAction *>("openSettings")->trigger();
        window->resize(800, 600);
        window->show();
        QTest::qWait(20);
        if (qEnvironmentVariableIsSet("QT_VIS_LIVE_AUDIO")) {
            settings_dialog->hide();
            app.presenter().state().file_configs.ui.show_raw_samples = true;
            app.presenter().update_subscription();
            tabs->setTabVisible(0, true);
            tabs->setTabVisible(4, true);
            tabs->setCurrentIndex(4);
            auto *editor = window->findChild<QDockWidget *>("dataFlowDock");
            if (editor->isHidden()) window->findChild<QAction *>("openPipeline")->trigger();
            require(editor && editor->isVisible(), "Live editor not visible");
            int fft_frames = 0, raw_frames = 0;
            float fft_peak = 0, raw_peak = 0;
            QObject::connect(&app, &CoreApplication::amplitudes_ready, &app, [&](const auto &values) {
                ++fft_frames;
                for (auto value : values)
                    fft_peak = std::max(fft_peak, std::abs(value));
            });
            QObject::connect(&app, &CoreApplication::raw_signal_ready, &app, [&](const auto &values) {
                ++raw_frames;
                for (auto value : values)
                    raw_peak = std::max(raw_peak, std::abs(value));
            });
            app.start_analyzer();
            app.start_loopback();
            QElapsedTimer audio_wait;
            audio_wait.start();
            while (raw_peak < 0.000001f && audio_wait.elapsed() < 20000)
                QTest::qWait(50);
            if (raw_peak < 0.000001f) {
                qCritical("LIVE_AUDIO: no non-silent system audio detected; check playback device");
                return 2;
            }
            require(app.pipeline().current_status("Capture").state == pipeline::State::Running,
                    "Real capture is not running");
            QUdpSocket udp;
            require(udp.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)), "Profile UDP receiver bind failed");
            int wled_packets = 0;
            QObject::connect(&udp, &QUdpSocket::readyRead, &app, [&] {
                while (udp.hasPendingDatagrams()) {
                    udp.receiveDatagram();
                    ++wled_packets;
                }
            });
            app.pipeline().start_wled(QHostAddress::LocalHost, udp.localPort(), QHostAddress::LocalHost);
            fft_frames = raw_frames = 0;
            fft_peak = raw_peak = 0;
            const auto before = app.pipeline().processed_frames();
            const auto graph = app.pipeline().graph();
            core::perf::enabled = true;
            QElapsedTimer duration, heartbeat;
            duration.start();
            heartbeat.start();
            std::vector<qint64> gaps;
            QTimer pulse;
            pulse.setInterval(16);
            pulse.setTimerType(Qt::PreciseTimer);
            QObject::connect(&pulse, &QTimer::timeout, &app, [&] {
                const auto gap = heartbeat.restart();
                gaps.push_back(gap);
                core::perf::duration(core::perf::Stage::GuiHeartbeat, gap * 1000000);
            });
            pulse.start();
            QEventLoop measurement;
            QTimer switch_view;
            switch_view.setInterval(2500);
            QObject::connect(&switch_view, &QTimer::timeout, &measurement,
                             [&] { tabs->setCurrentIndex(tabs->currentIndex() == 4 ? 0 : 4); });
            switch_view.start();
            QTimer::singleShot(10000, &measurement, &QEventLoop::quit);
            measurement.exec(); // Same event loop as the application; qWait polling distorts frame timings.
            pulse.stop();
            switch_view.stop();
            const auto elapsed = duration.elapsed() / 1000.0;
            const auto dsp_fps = (app.pipeline().processed_frames() - before) / elapsed;
            const auto trace_path = qEnvironmentVariable("QT_VIS_TRACE_FILE");
            if (!trace_path.isEmpty())
                require(save_performance_report(trace_path), "Cannot save performance trace");
            std::sort(gaps.begin(), gaps.end());
            require(!gaps.empty(), "No GUI heartbeat");
            const auto p95 = gaps[(gaps.size() - 1) * 95 / 100], worst = gaps.back();
            qInfo("LIVE_AUDIO: DSP %.2f fps, FFT delivery %.2f fps, raw delivery %.2f fps, GUI p95 %lld ms, worst %lld "
                  "ms, FFT peak %.6f, raw peak %.6f",
                  dsp_fps, fft_frames / elapsed, raw_frames / elapsed, static_cast<long long>(p95),
                  static_cast<long long>(worst), fft_peak, raw_peak);
            qInfo("LIVE_AUDIO: WLED receiver %.2f packets/s", wled_packets / elapsed);
            require(wled_packets / elapsed > 50, "GUI blocked WLED output");
            app.pipeline().stop_wled();
            require(fft_peak > 0.000001f && raw_peak > 0.000001f, "Live test received only silence");
            require(dsp_fps > 55 && dsp_fps < 65, "Real audio analysis cadence dropped");
            require(fft_frames / elapsed > 50 && raw_frames / elapsed > 20, "Real audio GUI deliveries lag behind DSP");
            require(p95 < 50 && worst < 200, "Real music stalls GUI with editor and graph");
            require(app.pipeline().graph() == graph, "Preview modified live graph");
            app.stop_loopback();
            app.stop_analyzer();
            editor->close();
            qInfo("LIVE_AUDIO: native real-music/editor validation passed");
            return 0;
        }
        auto wait_for = [](auto predicate, const char *message) {
            QElapsedTimer timer;
            timer.start();
            while (!predicate() && timer.elapsed() < 5000)
                QTest::qWait(5);
            require(predicate(), message);
        };
        auto runtime_state = [&app](const QString &component) {
            return app.presenter().client().snapshot().statuses.value(component).state;
        };
        auto wait_state = [&](const QString &component, pipeline::State state, const char *message) {
            wait_for([&] { return runtime_state(component) == state; }, message);
        };
        auto ensure_control = [&](QWidget *control) {
            auto *sections = settings->findChild<QTabWidget *>("settingsSections");
            for (int i = 0; i < sections->count(); ++i)
                if (sections->widget(i)->isAncestorOf(control))
                    sections->setCurrentIndex(i);
            auto *scroll = qobject_cast<QScrollArea *>(sections->currentWidget());
            scroll->ensureWidgetVisible(control);
            return scroll;
        };
        auto click = [&](const char *name) {
            auto *checkbox = settings->findChild<QCheckBox *>(name);
            require(checkbox, "Checkbox unavailable");
            ensure_control(checkbox);
            QTest::qWait(1);
            auto *viewport = ensure_control(checkbox)->viewport();
            require(viewport->rect().contains(checkbox->mapTo(viewport, QPoint(8, checkbox->height() / 2))),
                    "Checkbox is clipped by settings viewport");
            const auto before = checkbox->isChecked();
            QTest::mouseClick(checkbox, Qt::LeftButton, Qt::NoModifier, QPoint(8, checkbox->height() / 2));
            require(checkbox->isChecked() != before, "Mouse click did not toggle checkbox");
        };
        auto *port = settings->findChild<QSpinBox *>("wledPort");
        require(port && port->value() == 80, "Valid saved port is displayed incorrectly");
        for (int iteration = 0; iteration < 5; ++iteration) {
            window->findChild<QCheckBox *>("pipelineAnalyzer")->toggle();
            wait_state("Analyzer", pipeline::State::Running, "Analyzer click did not start worker");
            window->findChild<QCheckBox *>("pipelineCapture")->toggle();
            QTest::qWait(30);
            window->findChild<QCheckBox *>("pipelineAnalyzer")->toggle();
            wait_state("Analyzer", pipeline::State::Stopped, "Analyzer click did not release worker");
            window->findChild<QCheckBox *>("pipelineCapture")->toggle();
            wait_state("Capture", pipeline::State::Stopped, "Loopback click did not release worker");
        }
        const std::pair<const char *, int> charts[] = {
            {"showRawSamples", 0}, {"showWindowSignal", 1}, {"showWindowedSignal", 2}, {"showWindowFunction", 3},
            {"showFFT", 4},        {"showMel", 5},     {"showSpectrogram", 6}};
        for (auto [name, index] : charts) {
            const bool visible = tabs->isTabVisible(index);
            click(name);
            require(tabs->isTabVisible(index) != visible, "Chart toggle did not change tab visibility");
            click(name);
            require(tabs->isTabVisible(index) == visible, "Chart visibility not restored");
        }
        require(tabs->count() == 7 && !window->findChild<QToolBar *>() && window->findChild<QMenu *>("graphsMenu"),
                "Settings still occupy a graph tab or toolbar missing");
        auto *language=settings->findChild<QComboBox *>("interfaceLanguage");
        auto *sections=settings->findChild<QTabWidget *>("settingsSections");
        auto *graphs_menu=window->findChild<QMenu *>("graphsMenu");
        require(language && sections->count()==5 && graphs_menu->title()==QString::fromUtf8("Графики"),"Russian default/menu sections missing");
        const auto menu_actions=window->menuBar()->actions();
        require(menu_actions.size()==3 && menu_actions[0]->menu()==graphs_menu &&
            menu_actions[1]->menu()->objectName()=="wledMenu" && menu_actions[2]->menu()->objectName()=="settingsMenu",
            "Top-level menus must be Graphs, WLED, Settings");
        require(!settings->findChild<QCheckBox *>("enableAnalyzer") && !settings->findChild<QComboBox *>("captureSource") &&
            sections->widget(1)->findChild<QCheckBox *>("pipelineAutoApply") && sections->tabText(4)==ui::text("Controls"),
            "Processing duplicated outside graph or section order incorrect");
        auto *automatic=settings->findChild<QCheckBox *>("pipelineAutoApply");
        automatic->setChecked(false); require(!app._ctx.file_configs.ui.pipeline_auto_apply,"Auto apply preference not updated");
        automatic->setChecked(true);
        auto *fps_preference=settings->findChild<QCheckBox *>("showFPS");
        fps_preference->setChecked(false); require(window->findChild<QLabel *>("renderStatus")->isHidden(),"FPS preference ignored");
        fps_preference->setChecked(true);
        auto *theme=settings->findChild<QComboBox *>("interfaceTheme");
        require(theme,"Theme selector missing");
        const auto theme_revision=app.pipeline().graph_revision();
        theme->setCurrentIndex(1); QTest::qWait(100);
        require(QApplication::palette().color(QPalette::Window).lightness()<128 && app._ctx.file_configs.ui.theme=="dark",
            "Dark theme was not applied and stored");
        theme->setCurrentIndex(2); QTest::qWait(100);
        require(QApplication::palette().color(QPalette::Window).lightness()>128,"Light theme not applied");
        theme->setCurrentIndex(0);
        require(app.pipeline().graph_revision()==theme_revision,"Theme changed processing graph");
        auto *spectrogram_dock=window->findChild<QDockWidget *>("spectrogramDock");
        require(spectrogram_dock->titleBarWidget() && !spectrogram_dock->titleBarWidget()->findChild<QLabel *>(),
            "Graph dock repeats its tab title");
        spectrogram_dock->setFloating(true); QTest::qWait(100);
        require(!spectrogram_dock->titleBarWidget(),"Floating graph lost native window controls");
        spectrogram_dock->setFloating(false); QTest::qWait(100);
        require(spectrogram_dock->titleBarWidget(),"Docked graph controls not restored");
        const auto language_revision=app.pipeline().graph_revision();
        auto *same_spectrogram=window->findChild<SpectrogramGraph *>();
        language->setCurrentIndex(1); QTest::qWait(400);
        require(graphs_menu->title()=="Graphs" && sections->tabText(0)=="General" && window->findChild<SpectrogramGraph *>()==same_spectrogram,
                "Live language switch recreated views or failed to translate");
        require(app.pipeline().graph_revision()==language_revision,"Language switch applied DSP graph");
        language->setCurrentIndex(0); QTest::qWait(400);
        for (int i=0; i<5; ++i) {
            window->findChild<QAction *>(i==0 ? "openSettings" : QString("openSettingsSection%1").arg(i))->trigger();
            require(sections->currentIndex()==i,"Settings menu opened wrong section");
        }
        require(window->findChild<QCheckBox *>("fftAutoRange") && window->findChild<QComboBox *>("fftPanelChannel") &&
            window->findChild<QComboBox *>("melPanelMerge"),"Graph-specific controls missing");
        auto *auto_range=window->findChild<QCheckBox *>("fftAutoRange");
        auto_range->setChecked(false);
        require(!app._ctx.file_configs.ui.graph_views.value("fft").toObject().value("auto_range").toBool(true),"View preference not stored");
        require(app.pipeline().graph_revision()==language_revision,"Display scale applied DSP graph");
        auto_range->setChecked(true);
        auto *panel_channel=window->findChild<QComboBox *>("fftPanelChannel");
        const auto initial_channel=app._ctx.file_configs.core.spectrum_channel;
        panel_channel->setCurrentIndex((initial_channel+1)%3);
        wait_for([&] { return !app.presenter().graph_request_pending(); },"Graph panel command not acknowledged");
        require(app._ctx.file_configs.core.spectrum_channel==panel_channel->currentIndex(),
                "Graph panel and settings disagree after acknowledgement");
        panel_channel->setCurrentIndex(initial_channel);
        wait_for([&] { return !app.presenter().graph_request_pending(); },"Graph panel channel not restored");

        auto *fft_toggle = window->findChild<QDockWidget *>("fftDock")->toggleViewAction();
        const auto fft_visible = tabs->isTabVisible(4);
        fft_toggle->trigger();
        require(tabs->isTabVisible(4) != fft_visible &&
                    settings->findChild<QCheckBox *>("showFFT")->isChecked() == fft_toggle->isChecked(),
                "Toolbar graph toggle does not sync settings");
        fft_toggle->trigger();
        auto *analyzer_toggle = window->findChild<QCheckBox *>("pipelineAnalyzer");
        analyzer_toggle->setChecked(!analyzer_toggle->isChecked());
        wait_state("Analyzer", pipeline::State::Running, "Toolbar did not start analyzer");
        require(app._ctx.file_configs.core.run_analyser,"Pipeline start did not synchronize settings");
        analyzer_toggle->setChecked(!analyzer_toggle->isChecked());
        wait_state("Analyzer", pipeline::State::Stopped, "Toolbar did not stop analyzer");
        require(!app._ctx.file_configs.core.run_analyser,"Pipeline stop did not synchronize settings");
        auto assign = [&](const char *button_name, int key, int scan) {
            auto *button = settings->findChild<QPushButton *>(button_name);
            require(button, "Camera binding button missing");
            ensure_control(button);
            QTimer::singleShot(0, [&app, key, scan] {
                auto *dialog = QApplication::activeModalWidget();
                if (!dialog)
                    throw std::runtime_error("Key capture dialog not shown");
                QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier, scan, 0, 0);
                QApplication::sendEvent(dialog, &press);
            });
            QTest::mouseClick(button, Qt::LeftButton);
        };
        assign("bind_forward_0", 0x0428, 23); // Physical I, Russian letter Sh.
        assign("bind_forward_1", 0x041b, 37); // Physical K, Russian letter L.
        auto forward = app._ctx.file_configs.ui.camera_controls["forward"].toArray();
        require(forward.size() == 2 && forward[0].toObject()["scan"].toInt() == 23 &&
                    settings->findChild<QPushButton *>("bind_forward_0")->text() == "I",
                "Physical key rebinding failed");
        assign("bind_down_0", Qt::Key_I, 23);
        require(app._ctx.file_configs.ui.camera_controls["down"] == CameraControls::defaults()["down"],
                "Duplicate key binding accepted");
        ConfigManager controls_config(QString::fromLocal8Bit(path));
        FileSaveApplicationConfig controls_saved;
        require(controls_config.load(controls_saved) &&
                    controls_saved.ui.camera_controls == app._ctx.file_configs.ui.camera_controls,
                "Rebound controls not saved");
        auto *clear_alternative = settings->findChild<QPushButton *>("clear_forward");
        ensure_control(clear_alternative);
        QTest::mouseClick(clear_alternative, Qt::LeftButton);
        require(app._ctx.file_configs.ui.camera_controls["forward"].toArray().size() == 1,
                "Alternative binding not cleared");
        auto *default_keys = settings->findChild<QPushButton *>("restoreCameraKeys");
        ensure_control(default_keys);
        QTest::mouseClick(default_keys, Qt::LeftButton);
        require(app._ctx.file_configs.ui.camera_controls == CameraControls::defaults(),
                "Default controls not restored");
        if (QGuiApplication::platformName() != "offscreen") {
            click("showSpectrogram");
            auto *graph = window->findChild<SpectrogramGraph *>();
            settings_dialog->hide();
            tabs->setCurrentIndex(6);
            QTest::qWait(100);
            Q3DSurface *spectrogram = nullptr;
            for (auto *candidate : QGuiApplication::allWindows())
                if (candidate->objectName() == "spectrogramWindow")
                    spectrogram = qobject_cast<Q3DSurface *>(candidate);
            require(spectrogram, "Native spectrogram not initialized");
            auto *camera = spectrogram->scene()->activeCamera();
            const auto initial = graph->camera_state();
            camera->setZoomLevel(500);
            const QPointF wheel_position(spectrogram->width() / 2, spectrogram->height() / 2);
            QWheelEvent wheel(wheel_position, spectrogram->mapToGlobal(wheel_position.toPoint()), {}, QPoint(0, 120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QCoreApplication::sendEvent(spectrogram, &wheel);
            QTest::qWait(50);
            require(camera->zoomLevel() > 500, "Mouse wheel still stops at old zoom limit");
            graph->set_camera_state({{"horizontal", 35}, {"vertical", 45}, {"zoom", 1200}});
            require(camera->zoomLevel() == 1200, "Native camera still clamps close zoom to 500");
            graph->set_camera_state(initial);
            auto *splitter = graph->findChild<QSplitter *>("spectrogramSplitter");
            require(splitter && splitter->count() == 2, "Spectrogram resize handle missing");
            auto *handle = splitter->handle(1);
            const auto sizes_before = splitter->sizes();
            const auto center = handle->rect().center();
            QTest::mousePress(handle, Qt::LeftButton, Qt::NoModifier, center);
            QTest::mouseMove(handle, center - QPoint(60, 0), 20);
            QTest::mouseRelease(handle, Qt::LeftButton, Qt::NoModifier, center - QPoint(60, 0));
            QTest::qWait(30);
            require(splitter->sizes() != sizes_before, "Dragging divider did not resize spectrogram");
            const auto graph_width = splitter->widget(0)->width();
            window->resize(window->width() + 300, window->height());
            QTest::qWait(50);
            require(splitter->widget(0)->width() > graph_width, "Graph does not grow with window");
            auto move = [&](Qt::Key key) {
                QTest::keyPress(spectrogram, key);
                QTest::qWait(100);
                QTest::keyRelease(spectrogram, key);
                QTest::qWait(20);
            };
            graph->set_camera_controls(controls_saved.ui.camera_controls);
            QTest::keyPress(spectrogram, Qt::Key_I);
            QTest::keyPress(spectrogram, Qt::Key_K);
            QTest::keyRelease(spectrogram, Qt::Key_I);
            const auto before_alternative = camera->zoomLevel();
            QTest::qWait(80);
            require(camera->zoomLevel() > before_alternative, "Releasing primary key stopped held alternative");
            QTest::keyRelease(spectrogram, Qt::Key_K);
            graph->set_camera_controls(CameraControls::defaults());
            graph->set_camera_state(initial);
            move(Qt::Key_W);
            require(camera->zoomLevel() > initial["zoom"].toDouble(), "W did not move camera forward");
            const auto zoom = camera->zoomLevel();
            QTest::qWait(80);
            require(camera->zoomLevel() == zoom, "Camera kept moving after key release");
            move(Qt::Key_S);
            require(camera->zoomLevel() < zoom, "S did not move camera back");
            const auto target = camera->target();
            move(Qt::Key_D);
            require(camera->target() != target, "D did not strafe camera");
            move(Qt::Key_A);
            move(Qt::Key_Space);
            require(camera->target().y() > target.y(), "Space did not raise camera");
            move(Qt::Key_Control);
            const auto before_russian = camera->zoomLevel();
            QKeyEvent russian_press(QEvent::KeyPress, 0x0426, Qt::NoModifier, 17, 0, 0);
            QKeyEvent russian_release(QEvent::KeyRelease, 0x0426, Qt::NoModifier, 17, 0, 0);
            QApplication::sendEvent(spectrogram, &russian_press);
            QTest::qWait(100);
            QApplication::sendEvent(spectrogram, &russian_release);
            require(camera->zoomLevel() > before_russian, "Russian-layout W did not move native camera");
            const auto before_down = camera->target().y();
            QTest::keyPress(spectrogram, Qt::Key_Control);
            QTest::qWait(80);
            QTest::keyPress(spectrogram, Qt::Key_W, Qt::ControlModifier);
            QTest::qWait(80);
            QTest::keyRelease(spectrogram, Qt::Key_W, Qt::ControlModifier);
            QTest::keyRelease(spectrogram, Qt::Key_Control);
            require(camera->target().y() < before_down, "Ctrl did not lower camera while moving forward");
            QTest::keyClick(spectrogram, Qt::Key_Z);
            require(graph->camera_state() == initial, "Z did not reset camera to initial view");
            move(Qt::Key_W);
            auto *reset_position = graph->findChild<QPushButton *>("resetCamera");
            QTest::mouseClick(reset_position, Qt::LeftButton);
            require(graph->camera_state() == initial, "Camera reset button failed");
            QTest::keyPress(spectrogram, Qt::Key_W);
            tabs->setCurrentIndex(4);
            settings_dialog->show();
            QTest::qWait(100);
            const auto after_hide = camera->zoomLevel();
            QTest::qWait(100);
            require(camera->zoomLevel() == after_hide, "Camera kept moving on a hidden tab");
            QTest::qWait(600);
            ConfigManager camera_config(QString::fromLocal8Bit(path));
            FileSaveApplicationConfig restored;
            require(camera_config.load(restored) && restored.ui.camera == graph->camera_state(),
                    "Camera autosave failed");
            require(std::abs(restored.ui.spectrogram_side_panel_ratio - app._ctx.file_configs.ui.spectrogram_side_panel_ratio) <
                        1e-6,
                    "Divider position autosave failed");
            graph->set_camera_state(initial);
            graph->set_camera_state(restored.ui.camera);
            require(graph->camera_state() == restored.ui.camera, "Saved camera position did not restore");
            click("showSpectrogram");
        }
        click("enableWLED");
        wait_state("WLED", pipeline::State::Running, "WLED checkbox did not start server");
        const auto wled_revision = app.presenter().client().snapshot().statuses.value("WLED").revision;
        port->setValue(11989);
        wait_for(
            [&] {
                return runtime_state("WLED") == pipeline::State::Running &&
                       app.presenter().client().snapshot().statuses.value("WLED").revision > wled_revision;
            },
            "WLED port change failed");
        require(app._ctx.file_configs.wled_ar_server.port == 11989, "WLED port setting lost");
        click("enableWLED");
        port->setValue(11990);
        wait_state("WLED", pipeline::State::Stopped, "Changing port started disabled WLED");
        ConfigManager manager(QString::fromLocal8Bit(path));
        FileSaveApplicationConfig saved;
        require(manager.load(saved) && !saved.core.run_loopback && !saved.core.run_analyser &&
                    !saved.wled_ar_server.enabled && saved.wled_ar_server.port == 11990,
                "Settings were not persisted");
        click("enableAutosave");
        QFile persisted(QString::fromLocal8Bit(path));
        require(persisted.open(QIODevice::ReadOnly), "Cannot read saved settings");
        const auto previous_contents = persisted.readAll();
        persisted.close();
        click("showRawSamples");
        require(persisted.open(QIODevice::ReadOnly), "Cannot reread saved settings");
        require(persisted.readAll() == previous_contents, "Disabled autosave still wrote settings");
        persisted.close();
        click("enableAutosave");
        require(manager.load(saved) && saved.ui.autosave_enabled &&
                    saved.ui.show_raw_samples == app._ctx.file_configs.ui.show_raw_samples,
                "Enabling autosave did not save pending changes");
        auto *pipeline_dialog = window->findChild<QDockWidget *>("dataFlowDock");
        require(pipeline_dialog, "Pipeline editor dialog missing");
        if (pipeline_dialog->isHidden()) window->findChild<QAction *>("openPipeline")->trigger();
        QTest::qWait(20);
        auto *apply = pipeline_dialog->findChild<QPushButton *>("applyPipeline");
        require(apply && apply->isVisible(), "Pipeline apply control missing");
        QTest::mouseClick(apply, Qt::LeftButton);
        wait_for(
            [&] { return !pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text().startsWith(ui::text("Applying pipeline…")); },
            "Graph reply not delivered");
        require(pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"),
                "Default editor graph did not apply");
        auto valid = app.presenter().client().snapshot().graph;
        app._ctx.data_flow_graph->deleteConnection({0, 0, 1, 0});
        QTest::mouseClick(apply, Qt::LeftButton);
        wait_for(
            [&] { return !pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text().startsWith(ui::text("Applying pipeline…")); },
            "Graph reply not delivered");
        require(app.presenter().client().snapshot().graph == valid &&
                    pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text().startsWith(ui::text("Not applied: ")),
                "Invalid draft replaced running graph");
        require(manager.load(saved) && saved.pipeline_graph == valid && saved.scene_graph != valid,
                "Draft and applied graph were not saved separately");
        const auto rejected_draft = saved.scene_graph;
        const auto active_revision = app.presenter().client().snapshot().revision;
        QFile draft_file(QString::fromLocal8Bit(path));
        require(draft_file.open(QIODevice::ReadOnly), "Cannot read draft before load active");
        const auto draft_bytes = draft_file.readAll();
        draft_file.close();
        auto *load_active = pipeline_dialog->findChild<QAction *>("loadActivePipeline");
        require(load_active, "Load active pipeline control missing");
        load_active->trigger();
        QTest::qWait(450);
        require(app.presenter().client().snapshot().revision == active_revision &&
                    app.presenter().client().snapshot().graph == valid,
                "Load active mutated the engine");
        auto loaded_draft = app._ctx.data_flow_graph->save();
        loaded_draft["version"] = 1;
        require(canonical_editor_graph(loaded_draft) == canonical_editor_graph(valid),
                "Load active did not replace editor draft");
        require(manager.load(saved) && saved.scene_graph == rejected_draft, "Load active erased the saved draft");
        require(draft_file.open(QIODevice::ReadOnly), "Cannot read draft after load active");
        auto before_load = QJsonDocument::fromJson(draft_bytes).object();
        auto after_load = QJsonDocument::fromJson(draft_file.readAll()).object();
        // Workspace/view persistence is independent of applying or saving an editor draft.
        for (auto *document : {&before_load,&after_load}) {
            auto ui = document->value("ui").toObject(); ui.remove("workspace"); ui.remove("editor_view");
            (*document)["ui"] = ui;
        }
        require(before_load == after_load, "Load active changed processing settings or the saved draft");
        draft_file.close();
        click("showFFT");
        click("showFFT");
        require(manager.load(saved) && saved.scene_graph == rejected_draft,
                "Unrelated prefs autosave replaced the saved draft after Load active");
        pipeline_dialog->findChild<QAction *>("defaultPipeline")->trigger();
        wait_for(
            [&] { return pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"); },
            "Default graph reply not delivered");
        require(pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"),
                "Default pipeline recovery failed");
        auto *merge_model = app._ctx.data_flow_graph->delegateModel<AudioNodeModel>(2);
        auto *merge_combo = merge_model->embeddedWidget()->findChild<QComboBox *>("merge");
        merge_combo->setCurrentIndex(1);
        QTest::mouseClick(apply, Qt::LeftButton);
        wait_for(
            [&] { return pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"); },
            "Graph reply not delivered");
        require(manager.load(saved) && saved.pipeline_graph == app.presenter().client().snapshot().graph,
                "Node settings did not persist");
        require(app._ctx.file_configs.core.spectrum_merge == SpectrumMerge::Maximum,
                "Node merge and settings are out of sync");
        auto *source_model = app._ctx.data_flow_graph->delegateModel<AudioNodeModel>(0);
        auto *source_combo = source_model->embeddedWidget()->findChild<QComboBox *>("source");
        source_combo->setCurrentIndex(1);
        QTest::mouseClick(apply, Qt::LeftButton);
        wait_for(
            [&] { return pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"); },
            "Graph reply not delivered");
        require(app._ctx.file_configs.core.capture_source == 1,
                "Microphone node and settings are out of sync");
        require(manager.load(saved) && saved.core.capture_source == 1, "Microphone setting did not persist");
        app.start_loopback();
        QTest::qWait(100);
        wait_state("Capture", pipeline::State::Running, "Microphone capture did not start");
        require(app.presenter()
                        .client()
                        .snapshot()
                        .graph.value("nodes")
                        .toArray()[0]
                        .toObject()
                        .value("internal-data")
                        .toObject()
                        .value("source")
                        .toInt() == 1,
                "Microphone graph source incorrect");
        source_combo->setCurrentIndex(0);
        QTest::mouseClick(apply, Qt::LeftButton);
        wait_for(
            [&] { return pipeline_dialog->findChild<QLabel *>("pipelineValidation")->text() == ui::text("Pipeline applied"); },
            "Source graph reply not delivered");
        wait_state("Capture", pipeline::State::Running, "Live source change did not restart capture");
        require(app.presenter()
                        .client()
                        .snapshot()
                        .graph.value("nodes")
                        .toArray()[0]
                        .toObject()
                        .value("internal-data")
                        .toObject()
                        .value("source")
                        .toInt() == 0,
                "System output graph source incorrect");
        app.stop_loopback();
        QTest::qWait(450);
        tabs->setTabVisible(0, true);
        tabs->setTabVisible(4, true);
        QTest::qWait(450); // Establish presentation choices before the no-mutation baseline.
        const auto stable_graph = app.pipeline().graph();
        const auto stable_revision = app.pipeline().graph_revision();
        require(persisted.open(QIODevice::ReadOnly), "Cannot read config before preview test");
        const auto stable_config = persisted.readAll();
        persisted.close();
        app.start_analyzer();
        wait_state("Analyzer", pipeline::State::Running, "Analyzer not ready for preview stress");
        QElapsedTimer heartbeat;
        heartbeat.start();
        qint64 worst_gap = 0;
        int heartbeats = 0;
        QTimer pulse;
        pulse.setInterval(16);
        pulse.setTimerType(Qt::PreciseTimer);
        QObject::connect(&pulse, &QTimer::timeout, &app, [&] {
            worst_gap = std::max(worst_gap, heartbeat.restart());
            ++heartbeats;
        });
        pulse.start();
        pipeline::AudioFrame audio_frame;
        audio_frame.sample_rate = 48000;
        audio_frame.left.resize(4096);
        audio_frame.right.resize(4096);
        for (int i = 0; i < 4096; ++i)
            audio_frame.left[i] = audio_frame.right[i] = std::sin(i * 0.1f);
        for (int i = 0; i < 60; ++i) {
            tabs->setCurrentIndex(i < 30 ? 4 : 0);
            for (size_t sample = 0; sample < audio_frame.left.size(); ++sample)
                audio_frame.left[sample] = audio_frame.right[sample] =
                    (0.25f + 0.75f * (i % 31) / 30) * std::sin(sample * 0.1f + i * 0.017f);
            app.pipeline().feed(audio_frame);
            emit app.presenter().raw_signal_ready(audio_frame.left);
            QTest::qWait(20);
        }
        pulse.stop();
        require(heartbeats > 30 && worst_gap < 250, "GUI stalls with spectrum/audio and pipeline editor");
        require(app.pipeline().processed_frames() > 30, "GUI rendering starved DSP");
        require(app.pipeline().graph() == stable_graph && app.pipeline().graph_revision() == stable_revision,
                "Preview changed active pipeline");
        require(persisted.open(QIODevice::ReadOnly), "Cannot read config after preview test");
        auto stable_document = QJsonDocument::fromJson(stable_config).object();
        auto observed_document = QJsonDocument::fromJson(persisted.readAll()).object();
        for (auto *document : {&stable_document,&observed_document}) {
            auto ui = document->value("ui").toObject(); ui.remove("workspace"); ui.remove("editor_view");
            (*document)["ui"] = ui;
        }
        require(stable_document == observed_document, "Preview changed processing settings or saved draft");
        persisted.close();
        app.stop_analyzer();
        qInfo("Editor + FFT/audio responsiveness: worst event-loop gap %lld ms, %d heartbeats",
              static_cast<long long>(worst_gap), heartbeats);
        pipeline_dialog->close();
        qInfo("Settings and pipeline editor interaction tests passed");
    } catch (const std::exception &error) {
        qCritical("UI regression failed: %s", error.what());
        return 1;
    }
    return 0;
}
