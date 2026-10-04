#include "core_app/core_application.h"
#include "core_app/main_window.h"
#include "performance_report.h"
#include <QTemporaryDir>
#include <QFile>
#include <QTabWidget>
#include <QScreen>
#include <QEventLoop>
#include <QTimer>
#include <QtTest/QTest>
#include <thread>
#include <cmath>

int main() {
    QTemporaryDir temp;
    auto path=temp.filePath("settings.json").toLocal8Bit();
    QFile file(QString::fromLocal8Bit(path));
    if(!file.open(QIODevice::WriteOnly)) return 1;
    file.write(R"({"core":{"run_loopback":false,"run_analyser":false},"ui":{"show_surface":true}})");file.close();
    int argc=3;char name[]="render_tests",flag[]="--config";char *argv[]{name,flag,path.data(),nullptr};
    CoreApplication app(argc,argv);
    MainWindow *window=nullptr;
    for(auto *widget:QApplication::topLevelWidgets()) if(auto *candidate=qobject_cast<MainWindow *>(widget)) window=candidate;
    if(!window) return 1;
    auto *tabs=window->findChild<QTabWidget *>();
    Q3DSurface *native=nullptr;
    for(auto *candidate:QGuiApplication::allWindows()) if(candidate->objectName()=="surfaceWindow") native=qobject_cast<Q3DSurface *>(candidate);
    if(!native) { qCritical("Native 3D context unavailable");return 2; }
    tabs->setCurrentIndex(6);window->show();
    app.start_analyzer();
    std::jthread audio([&](std::stop_token stop) {
        uint64_t offset=0;
        pipeline::AudioFrame frame;frame.sample_rate=48000;frame.left.resize(240);frame.right.resize(240);
        auto deadline=std::chrono::steady_clock::now();
        while(!stop.stop_requested()) {
            for(size_t i=0;i<240;++i) frame.left[i]=frame.right[i]=(0.3f+0.2f*std::sin(offset/48000.0))*std::sin(2*3.141592653589793*750*(offset+i)/48000);
            offset+=240;app.pipeline().feed(frame);
            deadline+=std::chrono::milliseconds(5);std::this_thread::sleep_until(deadline);
        }
    });
    QTest::qWait(700);
    core::perf::enabled=true;
    bool passed=true;
    for(auto *screen:app.screens()) {
        tabs->setCurrentIndex(6);
        for(bool large:{false,true}) {
            window->windowHandle()->setScreen(screen);
            const auto bounds=screen->availableGeometry();
            window->setGeometry(QRect(bounds.topLeft()+QPoint(8,8),large ? bounds.size()-QSize(16,16) : QSize(800,600)));
            window->raise();window->activateWindow();
            QTest::qWait(350);
            std::vector<qint64> gaps;QElapsedTimer clock,gap_clock;clock.start();gap_clock.start();
            const auto before=app.pipeline().processed_frames();
            const auto &renders=core::perf::counters[static_cast<size_t>(core::perf::Stage::SurfaceRender)];
            const auto render_before=renders.count.load(),time_before=renders.total.load();
            QEventLoop loop;QTimer camera;camera.setTimerType(Qt::PreciseTimer);camera.setInterval(16);
            QObject::connect(&camera,&QTimer::timeout,&loop,[&] {
                gaps.push_back(gap_clock.restart());
                auto *c=native->scene()->activeCamera();const auto t=clock.elapsed()/1000.0;
                c->setXRotation(45+35*std::sin(t));c->setYRotation(35+15*std::sin(t*0.7));
                c->setTarget(QVector3D(0.3f*std::sin(t*1.7),0,-0.3f*std::cos(t*1.7)));
            });camera.start();QTimer::singleShot(3500,&loop,&QEventLoop::quit);loop.exec();camera.stop();
            const auto seconds=clock.elapsed()/1000.0,rate=(app.pipeline().processed_frames()-before)/seconds;
            const auto rendered=renders.count.load()-render_before;
            const auto render_ms=rendered ? (renders.total.load()-time_before)/double(rendered)/1000000.0 : 0;
            std::sort(gaps.begin(),gaps.end());
            const auto p95=gaps.empty() ? 0 : gaps[(gaps.size()-1)*95/100],worst=gaps.empty() ? 0 : gaps.back();
            qInfo("RENDER screen=%s refresh=%.1f dpr=%.2f size=%dx%d DSP=%.2f rendered=%.2f/s render_mean=%.3fms heartbeat_p95=%lldms max=%lldms shadow=%d swap=%d",
                qPrintable(screen->name()),screen->refreshRate(),native->devicePixelRatio(),native->width(),native->height(),rate,rendered/seconds,render_ms,
                static_cast<long long>(p95),static_cast<long long>(worst),int(native->shadowQuality()),native->format().swapInterval());
            passed &= rate>55 && rate<65 && rendered/seconds>45 && rendered/seconds<61 && p95<30 && worst<100;
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
    audio.request_stop();audio.join();app.stop_analyzer();
    const auto trace=qEnvironmentVariable("QT_VIS_TRACE_FILE");
    if(!trace.isEmpty()) passed &= save_performance_report(trace);
    core::perf::enabled=false;
    qInfo("Native multi-screen render check %s",passed ? "passed" : "failed");return passed ? 0 : 1;
}
