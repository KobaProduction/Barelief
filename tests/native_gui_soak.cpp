#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "process_memory.h"
#include "performance_report.h"
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QTest>
#include <QDockWidget>
#include <QTimer>
#include <QQuaternion>
#include <thread>
#include <chrono>
#include <cmath>
int main(int argc, char **argv) {
    const int duration = argc > 1 ? QString::fromLocal8Bit(argv[1]).toInt() : 1800000;
    const QString report_path = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString{};
    if (duration < 1000 || duration > 3600000)
        return 2;
    QTemporaryDir dir;
    auto path = dir.filePath("silent-native.json").toLocal8Bit();
    auto graph = pipeline::GraphProgram::test_signal_defaults();
    QFile file(QString::fromLocal8Bit(path));
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(QJsonDocument(QJsonObject{{"core", QJsonObject{{"run_loopback", true}, {"run_analyser", true}}},
                                         {"pipeline_graph", graph},
                                         {"scene_graph", graph},
                                         {"ui", QJsonObject{{"autosave_enabled", false}}}})
                   .toJson());
    file.close();
    char name[] = "native_gui_soak", flag[] = "--config";
    char *args[]{name, flag, path.data(), nullptr};
    int count = 3;
    CoreApplication app(count, args);
    if (app.platformName() == "offscreen" || app.platformName() == "minimal")
        return 2;
    MainWindow *window = nullptr;
    for (auto *widget : QApplication::topLevelWidgets())
        if (auto *candidate = qobject_cast<MainWindow *>(widget))
            window = candidate;
    if (!window)
        return 1;
    window->resize(1200, 850);
    window->show();
    QTest::qWait(1500);
    auto *graph_view = window->findChild<SpectrogramGraph *>();
    auto *gl = graph_view->findChild<QOpenGLWidget *>("spectrogramContainer");
    if (!gl || !gl->isValid() || !gl->property("rendererReady").toBool())
        return 1;
    // A blocked GUI must not stop the independently owned producer.
    const auto before_block = app.pipeline().processed_frames();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const auto blocked_frames = app.pipeline().processed_frames() - before_block;
    if (blocked_frames < 250)
        return 1;
    QTest::qWait(500);
    core::perf::enabled = true;
    const auto memory_before = resident_memory();
    auto peak_memory = memory_before;
    const auto before = app.pipeline().processed_frames();
    QElapsedTimer clock, gap;
    clock.start();
    gap.start();
    bool passed = true;
    uint64_t views = 0;
    float peak = 0;
    QObject::connect(&app.presenter(), &GuiPresenter::node_views_ready, &app, [&](const auto &frames) {
        ++views;
        for (const auto &f : frames)
            for (float value : f.values)
                peak = std::max(peak, std::abs(value));
    });
    QTimer heartbeat, report, camera;
    heartbeat.setInterval(17);
    heartbeat.setTimerType(Qt::PreciseTimer);
    report.setInterval(10000);
    camera.setInterval(34);
    camera.setTimerType(Qt::PreciseTimer);
    QObject::connect(&heartbeat, &QTimer::timeout, &app,
                     [&] { core::perf::duration(core::perf::Stage::GuiHeartbeat, gap.restart() * 1000000); });
    QObject::connect(&camera, &QTimer::timeout, &app, [&] {
        const auto t = clock.elapsed() / 1000.0;
        const auto orientation = QQuaternion::fromEulerAngles(-30 + 15 * std::sin(t * .3), 30 * std::sin(t * .7), 0);
        graph_view->set_camera_state(
            {{"version", 2},
             {"position", QJsonArray{2 * std::sin(t * .1), 1 + 1.5 * std::sin(t * .08), 3}},
             {"quaternion", QJsonArray{orientation.scalar(), orientation.x(), orientation.y(), orientation.z()}}});
    });
    QObject::connect(&report, &QTimer::timeout, &app, [&] {
        const auto diagnostics = app.pipeline().capture_diagnostics();
        auto memory = resident_memory();
        peak_memory = std::max(memory, peak_memory);
        passed &= diagnostics.buffered_frames <= diagnostics.capacity_frames && app.pipeline().history_count() <= 400 &&
                  gl->isValid() && gl->property("rendererReady").toBool();
        qInfo("GUI_SOAK elapsed_ms=%lld dsp=%llu views=%llu rss_mib=%.2f queue=%zu/%zu drops=%llu history=%zu "
              "render_fps=%.2f peak=%.7f",
              clock.elapsed(), static_cast<unsigned long long>(app.pipeline().processed_frames() - before),
              static_cast<unsigned long long>(views), memory / 1048576.0, diagnostics.buffered_frames,
              diagnostics.capacity_frames, static_cast<unsigned long long>(diagnostics.dropped_frames),
              app.pipeline().history_count(), graph_view->display_fps(), peak);
    });
    heartbeat.start();
    report.start();
    camera.start();
    QTimer::singleShot(duration, &app, &QCoreApplication::quit);
    const auto exit_status = app.exec();
    passed &= exit_status == 0;
    heartbeat.stop();
    report.stop();
    camera.stop();
    const auto seconds = clock.elapsed() / 1000.0, rate = (app.pipeline().processed_frames() - before) / seconds;
    passed &= rate > 55 && rate < 65 && peak_memory - memory_before < 64 * 1048576 && peak > 1e-7 &&
              views > uint64_t(seconds * 30);
    if (!report_path.isEmpty())
        passed &= save_performance_report(report_path);
    core::perf::enabled = false;
    qInfo("GUI_SOAK_RESULT passed=%d duration_s=%.3f DSP_fps=%.3f blocked_gui_frames=%llu peak_growth_mib=%.3f "
          "views=%llu",
          passed, seconds, rate, static_cast<unsigned long long>(blocked_frames),
          (peak_memory - memory_before) / 1048576.0, static_cast<unsigned long long>(views));
    return passed ? 0 : 1;
}
