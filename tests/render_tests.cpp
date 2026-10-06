#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "core_app/gui_event_profile.h"
#include "performance_report.h"
#include "process_memory.h"
#include "native_swap_probe.h"
#include "native_session_probe.h"
#include <QTemporaryDir>
#include <QFile>
#include <QPointer>
#include <QDockWidget>
#include <QAction>
#include "dock_panels.h"
#include <QScreen>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QJsonArray>
#include <QOpenGLFunctions>
#include <QUdpSocket>
#include <QNetworkDatagram>
#include <QEventLoop>
#include <QTimer>
#include <QtTest/QTest>
#include <thread>
#include <cmath>
#include <QJsonDocument>

int main() {
    const auto soak_duration = qEnvironmentVariableIntValue("QT_VIS_CAPTURE_SOAK_MS");
    const auto silent_feed = qEnvironmentVariableIntValue("QT_VIS_RENDER_SILENCE") != 0;
    const auto require_signal = qEnvironmentVariableIntValue("QT_VIS_REQUIRE_SIGNAL") != 0;
    if (require_signal && !soak_duration) {
        qCritical("QT_VIS_REQUIRE_SIGNAL requires a capture soak"); return 2;
    }
    if (soak_duration < 0 || soak_duration > 3600000) {
        qCritical("QT_VIS_CAPTURE_SOAK_MS must be between 0 and 3600000"); return 2;
    }
    QTemporaryDir temp;
    auto path=temp.filePath("settings.json").toLocal8Bit();
    QFile file(QString::fromLocal8Bit(path));
    if(!file.open(QIODevice::WriteOnly)) return 1;
    auto graph = qEnvironmentVariableIntValue("QT_VIS_DEVICE_CAPTURE")
        ? pipeline::GraphProgram::defaults() : pipeline::GraphProgram::test_signal_defaults();
    if (silent_feed && !qEnvironmentVariableIntValue("QT_VIS_DEVICE_CAPTURE")) {
        auto nodes = graph["nodes"].toArray(); auto source = nodes[0].toObject();
        auto parameters = source["internal-data"].toObject(); parameters["amplitude"] = 0;
        source["internal-data"] = parameters; nodes[0] = source; graph["nodes"] = nodes;
    }
    file.write(QJsonDocument(QJsonObject{{"core",QJsonObject{{"run_loopback",false},{"run_analyser",false}}},
        {"ui",QJsonObject{{"show_spectrogram",true}}},{"pipeline_graph",graph},{"scene_graph",graph}}).toJson()); file.close();
    int argc=3;char name[]="render_tests",flag[]="--config";char *argv[]{name,flag,path.data(),nullptr};
    CoreApplication app(argc,argv);
    uint64_t signal_spectra = 0;
    QObject::connect(&app, &CoreApplication::amplitudes_ready, &app, [&](const auto &values) {
        if (std::any_of(values.begin(), values.end(), [](float value) { return value > 0.0000001f; }))
            ++signal_spectra;
    });
    NativeSwapProbe swap_probe;
    if (qEnvironmentVariableIntValue("QT_VIS_SWAP_PROBE") && !swap_probe.install()) {
        qCritical("Native SwapBuffers probe is unavailable on this platform/plugin"); return 2;
    }
    MainWindow *window=nullptr;
    for(auto *widget:QApplication::topLevelWidgets()) if(auto *candidate=qobject_cast<MainWindow *>(widget)) window=candidate;
    if(!window) return 1;
    DockPanels panel_views(window); auto *tabs=&panel_views;
    auto *spectrogram = window->findChild<SpectrogramGraph *>();
    auto *native=spectrogram->findChild<QOpenGLWidget*>("spectrogramContainer");
    if(!native) { qCritical("Native 3D context unavailable");return 2; }
    if (auto *context = native->context()) {
        const auto format = context->format();
        qInfo("GRAPHICS api=%s version=%d.%d samples=%d alpha=%d depth=%d stencil=%d", context->isOpenGLES() ? "OpenGLES" : "OpenGL",
              format.majorVersion(), format.minorVersion(), format.samples(), format.alphaBufferSize(),
              format.depthBufferSize(), format.stencilBufferSize());
        if (QOpenGLContext::currentContext() == context)
            qInfo("GRAPHICS vendor=%s renderer=%s", context->functions()->glGetString(GL_VENDOR),
                  context->functions()->glGetString(GL_RENDERER));
    }
    if (qEnvironmentVariableIntValue("QT_VIS_QA_TOPMOST")) {
        window->setWindowFlag(Qt::WindowStaysOnTopHint);
        qInfo("SESSION test_window_topmost=1");
    }
    tabs->setCurrentIndex(6);window->show();
    NativeSessionProbe session_probe(window);
    app.start_analyzer();
    app.start_loopback(); // Same configured source factory as GUI/console; no playback helper.
    QTest::qWait(700);
    if(qEnvironmentVariableIntValue("QT_VIS_SHORT_PROFILE_MS")>0) {
        const int duration=std::clamp(qEnvironmentVariableIntValue("QT_VIS_SHORT_PROFILE_MS"),1000,30000);
        // In-memory eight-tone TestSignal; no playback device is created.
        // Use the current user's 50x400 visualization dimensions, without
        // reading or writing their configuration or resetting their workspace.
        window->resize(1400,900);QTest::qWait(7000); // Fill the bounded 400-frame history.
        const auto before=app.pipeline().processed_frames();
        const auto frames=native->property("renderedFrames").toULongLong();
        QElapsedTimer clock;clock.start();QEventLoop loop;
        core::perf::enabled=true;
        QTimer::singleShot(duration,&loop,&QEventLoop::quit);loop.exec();
        const double seconds=clock.elapsed()/1000.;
        const double dsp=(app.pipeline().processed_frames()-before)/seconds;
        const double fps=(native->property("renderedFrames").toULongLong()-frames)/seconds;
        qInfo("SHORT_PIPELINE eight_tones=1 DSP=%.2f/s FPS=%.2f size=%dx%d DPR=%.2f history=%zu",dsp,fps,
            native->width(),native->height(),native->devicePixelRatioF(),app.pipeline().history_count());
        app.stop_loopback();app.stop_analyzer();
        const bool traceSaved=save_performance_report(qEnvironmentVariable("QT_VIS_TRACE_FILE"));
        const bool sessionValid=session_probe.report();
        return traceSaved && sessionValid && dsp>55 && dsp<65 && fps>50 && fps<61 ? 0 : 1;
    }
    const auto camera_state = spectrogram->camera_state();
    QPointer<QOpenGLWidget> original_native = native;
    for (int round = 0; round < 3; ++round) {
        for (auto *dock : window->findChildren<QDockWidget *>()) {
            auto *view = dock->widget();
            dock->show(); dock->raise(); dock->setFloating(true); QTest::qWait(60);
            dock->close(); dock->toggleViewAction()->trigger(); QTest::qWait(30);
            dock->setFloating(false);
            if (dock->widget() != view || original_native != native || !original_native) {
                qCritical("Docking recreated a view or its native Spectrogram"); return 1;
            }
        }
        window->findChild<QAction *>("resetWorkspace")->trigger(); QTest::qWait(100);
        if (spectrogram->camera_state() != camera_state) { qCritical("Docking changed camera state"); return 1; }
    }
    QUdpSocket udp;
    if (!udp.bind(QHostAddress(QHostAddress::LocalHost), quint16(0))) return 1;
    app.pipeline().start_wled(QHostAddress::LocalHost, udp.localPort(), QHostAddress::LocalHost);
    const auto before_block = app.pipeline().processed_frames();
    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (app.pipeline().processed_frames() - before_block < 270 || app.pipeline().history_count() < 270 ||
        !udp.hasPendingDatagrams()) {
        qCritical("Native GUI stall stopped DSP, history or UDP output"); return 1;
    }
    // Keep the real UDP output active during rendering/resize measurements.
    // Drain the local receiver so socket capacity cannot mask delivery issues.
    const auto drain_udp = [&] {
        while (udp.hasPendingDatagrams()) udp.receiveDatagram();
    };
    drain_udp();
    QObject::connect(&udp, &QUdpSocket::readyRead, &udp, drain_udp);
    QTest::qWait(100);
    core::perf::enabled=true;
    const bool profile_events = qEnvironmentVariableIntValue("QT_VIS_GUI_EVENT_PROFILE") != 0;
    QJsonArray resize_events;
    bool passed=true;
    for(auto *screen:app.screens()) {
        tabs->setCurrentIndex(6);
        for(bool large:{false,true}) {
            QElapsedTimer placement; placement.start();
            window->windowHandle()->setScreen(screen);
            const auto screen_ms = placement.nsecsElapsed() / 1000000.0;
            const auto bounds=screen->availableGeometry();
            window->setGeometry(QRect(bounds.topLeft()+QPoint(8,8),large ? bounds.size()-QSize(16,16) : QSize(800,600)));
            const auto geometry_ms = placement.nsecsElapsed() / 1000000.0 - screen_ms;
            window->raise();window->activateWindow();
            qInfo("WINDOW_PLACEMENT screen=%s set_screen=%.3fms geometry=%.3fms total=%.3fms",
                qPrintable(screen->name()), screen_ms, geometry_ms, placement.nsecsElapsed() / 1000000.0);
            QTest::qWait(350);
            std::vector<qint64> gaps;QElapsedTimer clock,gap_clock;clock.start();gap_clock.start();
            const auto before=app.pipeline().processed_frames();
            const auto &renders=core::perf::counters[static_cast<size_t>(core::perf::Stage::SpectrogramRender)];
            const auto render_before=renders.count.load(),time_before=renders.total.load();
            QEventLoop loop;QTimer camera;camera.setTimerType(Qt::PreciseTimer);camera.setInterval(16);
            QObject::connect(&camera,&QTimer::timeout,&loop,[&] {
                gaps.push_back(gap_clock.restart());
                const auto t=clock.elapsed()/1000.0;
                spectrogram->set_camera_state({{"horizontal",45+35*std::sin(t)},{"vertical",35+15*std::sin(t*.7)},{"zoom",155}});
            });camera.start();QTimer::singleShot(3500,&loop,&QEventLoop::quit);loop.exec();camera.stop();
            const auto seconds=clock.elapsed()/1000.0,rate=(app.pipeline().processed_frames()-before)/seconds;
            const auto rendered=renders.count.load()-render_before;
            const auto render_ms=rendered ? (renders.total.load()-time_before)/double(rendered)/1000000.0 : 0;
            std::sort(gaps.begin(),gaps.end());
            const auto p95=gaps.empty() ? 0 : gaps[(gaps.size()-1)*95/100],worst=gaps.empty() ? 0 : gaps.back();
            qInfo("RENDER screen=%s refresh=%.1f dpr=%.2f size=%dx%d DSP=%.2f rendered=%.2f/s render_mean=%.3fms heartbeat_p95=%lldms max=%lldms shadow=%d swap=%d",
                qPrintable(screen->name()),screen->refreshRate(),native->devicePixelRatio(),native->width(),native->height(),rate,rendered/seconds,render_ms,
                static_cast<long long>(p95),static_cast<long long>(worst),int(0),native->format().swapInterval());
            passed &= rate>55 && rate<65 && rendered/seconds>45 && rendered/seconds<61 && p95<30 && worst<100;
        }
        // Resize/expose events must share the camera/data frame budget. Flood
        // real window resizes faster than 60 Hz rather than counting only data updates.
        const auto margins = window->windowHandle()->frameMargins();
        const auto available = screen->availableGeometry();
        const QSize small = window->minimumSize().expandedTo(QSize(800, 600));
        const QSize large = (available.size() - QSize(margins.left() + margins.right() + 24,
            margins.top() + margins.bottom() + 24)).expandedTo(small);
        window->resize(small); QTest::qWait(350);
        QSize actual_min = window->size(), actual_max = window->size();
        const auto &resize_renders = core::perf::counters[static_cast<size_t>(core::perf::Stage::SpectrogramRender)];
        const auto resize_before = resize_renders.count.load();
        QElapsedTimer resize_clock; resize_clock.start();
        const auto groups_before = gui_event_profile::groups;
        gui_event_profile::enabled = profile_events;
        QTimer resize_timer; resize_timer.setTimerType(Qt::PreciseTimer); resize_timer.setInterval(5);
        QEventLoop resize_loop; int resize_count = 0;
        QObject::connect(&resize_timer, &QTimer::timeout, &resize_loop, [&] {
            core::perf::Scope resize_trace(core::perf::Stage::WindowResize);
            // Alternate substantial width-only and height-only changes, not
            // tiny movements that leave backing-store/axis work untested.
            const int phase = ++resize_count % 4;
            window->resize((phase == 1 || phase == 2) ? large.width() : small.width(),
                           (phase == 2 || phase == 3) ? large.height() : small.height());
            actual_min = actual_min.boundedTo(window->size());
            actual_max = actual_max.expandedTo(window->size());
        });
        resize_timer.start(); QTimer::singleShot(2000, &resize_loop, &QEventLoop::quit);
        resize_loop.exec(); resize_timer.stop();
        gui_event_profile::enabled = false;
        if (profile_events) {
            for (size_t i = 0; i < gui_event_profile::group_count; ++i) {
                const auto &group = gui_event_profile::groups[i];
                const auto &previous = groups_before[i];
                if (group.count == previous.count) continue;
                resize_events.append(QJsonObject{{"screen", screen->name()},
                    {"receiver", group.receiver.data()}, {"type", group.type},
                    {"count", double(group.count - previous.count)},
                    {"inclusive_ms", (group.inclusive - previous.inclusive) / 1000000.0},
                    {"exclusive_ms", (group.exclusive - previous.exclusive) / 1000000.0}});
            }
        }
        const auto resize_rate = (resize_renders.count.load() - resize_before) / (resize_clock.elapsed() / 1000.0);
        qInfo("RESIZE_RENDER screen=%s requests=%d rendered=%.2f/s actual_range=%dx%d..%dx%d requested_range=%dx%d..%dx%d",
            qPrintable(screen->name()), resize_count, resize_rate, actual_min.width(), actual_min.height(),
            actual_max.width(), actual_max.height(), small.width(), small.height(), large.width(), large.height());
        passed &= resize_count > 100 && resize_rate > 0 && resize_rate < 61 &&
                  actual_max.width() - actual_min.width() >= 100 &&
                  actual_max.height() - actual_min.height() >= 50;
        QTest::qWait(50);
        auto *container = spectrogram->findChild<QWidget *>("spectrogramContainer");
        if (!container || native->size() != container->size()) {
            qCritical("Native Spectrogram did not converge to its resized container"); return 1;
        }
        tabs->setTabVisible(4,true);tabs->setCurrentIndex(4);QTest::qWait(350);
        QElapsedTimer fft_clock,heartbeat;fft_clock.start();heartbeat.start();std::vector<qint64> fft_gaps;
        QTimer pulse;pulse.setInterval(16);pulse.setTimerType(Qt::PreciseTimer);QEventLoop fft_loop;
        QObject::connect(&pulse,&QTimer::timeout,&fft_loop,[&] { fft_gaps.push_back(heartbeat.restart()); });
        pulse.start();QTimer::singleShot(2500,&fft_loop,&QEventLoop::quit);fft_loop.exec();pulse.stop();
        std::sort(fft_gaps.begin(),fft_gaps.end());
        const auto fft_p95=fft_gaps.empty() ? 0 : fft_gaps[(fft_gaps.size()-1)*95/100];
        qInfo("FFT_RENDER screen=%s size=%dx%d heartbeat_p95=%lldms max=%lldms",qPrintable(screen->name()),window->width(),window->height(),static_cast<long long>(fft_p95),static_cast<long long>(fft_gaps.empty() ? 0 : fft_gaps.back()));
        passed &= !fft_gaps.empty() && fft_p95<40 && fft_gaps.back()<100;
    }
    if (soak_duration) {
        uint64_t signal_intervals = 0, checked_intervals = 0, previous_signal_spectra = signal_spectra;
        tabs->setCurrentIndex(6);
        const auto before = app.pipeline().processed_frames();
        const auto memory_before = resident_memory();
        auto memory_peak = memory_before;
        QElapsedTimer elapsed; elapsed.start();
        QEventLoop loop; QTimer report, camera;
        report.setInterval(1000); camera.setInterval(16); camera.setTimerType(Qt::PreciseTimer);
        QObject::connect(&camera, &QTimer::timeout, &loop, [&] {
            const auto t = elapsed.elapsed() / 1000.0;
            spectrogram->set_camera_state({{"horizontal",45+35*std::sin(t)},{"vertical",35+15*std::sin(t*.7)},{"zoom",155}});

        });
        QObject::connect(&report, &QTimer::timeout, &loop, [&] {
            ++checked_intervals;
            if (signal_spectra != previous_signal_spectra) ++signal_intervals;
            previous_signal_spectra = signal_spectra;
            const auto diagnostics = app.pipeline().capture_diagnostics();
            const auto memory = resident_memory(); memory_peak = std::max(memory_peak, memory);
            const auto history = app.pipeline().history_count();
            passed &= diagnostics.buffered_frames <= diagnostics.capacity_frames && history <= 400;
            qInfo() << "NATIVE_CAPTURE_SOAK" << elapsed.elapsed() << "ms RSS" << memory / 1048576.0
                    << "MiB queued" << diagnostics.buffered_frames << "dropped" << diagnostics.dropped_frames
                    << "gaps" << diagnostics.discontinuities << "history" << history
                    << "signal intervals" << signal_intervals << "/" << checked_intervals
                    << "dispatch ms" << diagnostics.dispatch_latency_ns / 1000000.0;
        });
        report.start(); camera.start(); QTimer::singleShot(soak_duration, &loop, &QEventLoop::quit); loop.exec();
        report.stop(); camera.stop();
        const auto seconds = elapsed.elapsed() / 1000.0;
        const auto rate = (app.pipeline().processed_frames() - before) / seconds;
        passed &= rate > 55 && rate < 65 && memory_peak - memory_before < 64 * 1048576 &&
                  app.pipeline().current_status("Capture").state == pipeline::State::Running;
        if (require_signal)
            passed &= checked_intervals >= uint64_t(soak_duration / 1000) * 9 / 10 &&
                      signal_intervals * 100 >= checked_intervals * 95;
        qInfo("Native capture soak DSP=%.2f/s peak_RSS_growth=%.2fMiB", rate, (memory_peak - memory_before) / 1048576.0);
        app.stop_loopback();
    }
    app.stop_loopback();
    app.stop_analyzer();
    app.pipeline().stop_wled();
    const auto trace=qEnvironmentVariable("QT_VIS_TRACE_FILE");
    gui_event_profile::enabled = false;
    QJsonArray gui_events;
    for (const auto &event : resize_events) gui_events.append(event);
    for (size_t i = 0; i < gui_event_profile::group_count; ++i) {
        const auto &group = gui_event_profile::groups[i];
        gui_events.append(QJsonObject{{"receiver", group.receiver.data()}, {"type", group.type},
            {"count", double(group.count)}, {"inclusive_ms", group.inclusive / 1000000.0},
            {"exclusive_ms", group.exclusive / 1000000.0}, {"max_ms", group.maximum / 1000000.0}});
    }
    qInfo("GUI_EVENT_PROFILE groups=%zu overflow=%llu", gui_event_profile::group_count,
          static_cast<unsigned long long>(gui_event_profile::overflow));
    if(!trace.isEmpty()) passed &= save_performance_report(trace, gui_events);
    core::perf::enabled=false;
    swap_probe.report();
    if (!session_probe.report()) {
        qCritical("Native graphics measurement invalid: desktop was inactive or session could not be verified");
        return 2;
    }
    const auto screenshot = qEnvironmentVariable("QT_VIS_SCREENSHOT_FILE");
    if (!screenshot.isEmpty()) {
        // Parent-window capture can omit native children. Export the same Spectrogram
        // separately, outside all measured intervals, using its own renderer.
        if (!window->screen()->grabWindow(window->winId()).save(screenshot) ||
            !native->grabFramebuffer().save(screenshot + ".spectrogram.png")) {
            qCritical("Cannot save test-window/Spectrogram images"); return 2;
        }
    }
    qInfo("Native multi-screen render check %s",passed ? "passed" : "failed");return passed ? 0 : 1;
}
