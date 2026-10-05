#include "core_app/core_application.h"
#include "core/latest_delivery.h"
#include "visualization/spectrograms/spectrogram_graph.h"
#include <QJsonArray>
#include <thread>
#include "aggregator/aggregator_server.h"
#include "core/network/tcp_packet_socket.h"
#include "visualization/charts/line.h"
#include "visualization/charts/freq.h"
#include <QTemporaryDir>
#include <QFile>
#include <QEventLoop>
#include <QTimer>
#include <QLineSeries>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <cstring>

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
static void pump(int ms = 30) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}
int main(int, char **) {
    QTemporaryDir temp;
    auto config_path = temp.filePath("startup.json").toLocal8Bit();
    QFile file(QString::fromLocal8Bit(config_path));
    if (!file.open(QIODevice::WriteOnly)) return 1;
    file.write(R"({"core":{"run_loopback":false,"run_analyser":false},"ui":{"show_spectrogram":false},"scene_graph":{"nodes":[{"id":1,"internal-data":{"model-name":"RemovedModel"}}],"connections":[{"outNodeId":1,"inNodeId":9}]}})");
    file.close();
    int argc = 3;
    char name[] = "runtime_tests", flag[] = "--config";
    char *argv[] = {name, flag, config_path.data(), nullptr};
    CoreApplication app(argc, argv);
    try {
        int delivered = 0, newest = -1;
        auto delivery = std::make_shared<LatestDelivery<int>>(&app, [&](int value) { ++delivered; newest = value; });
        std::thread producer([&] { for (int i = 0; i < 10000; ++i) delivery->publish(i); });
        producer.join(); // Deliberately leave the GUI event loop blocked during production.
        require(delivered == 0, "Snapshot delivered outside GUI event loop");
        pump(5);
        require(delivered == 1 && newest == 9999, "GUI queue is unbounded or lost latest snapshot");
        auto *temporary_receiver=new QObject;
        int cancelled_delivery=0;
        auto cancelled=std::make_shared<LatestDelivery<int>>(temporary_receiver,[&](int){++cancelled_delivery;});
        cancelled->publish(1);
        cancelled->cancel();
        delete temporary_receiver;
        std::thread late_producer([&]{for(int i=0;i<10000;++i)cancelled->publish(i);});
        late_producer.join();pump(5);
        require(cancelled_delivery==0,"Cancelled delivery used a destroyed receiver");
        int polled_count = 0, polled_latest = -1;
        auto polled = std::make_shared<LatestDelivery<int>>(&app, [&](int value) {
            ++polled_count; polled_latest = value;
        });
        polled->start_polling();
        std::thread polled_producer([&] { for (int i = 0; i < 10000; ++i) polled->publish(i); });
        polled_producer.join();
        require(polled_count == 0, "Polling producer invoked the GUI callback");
        pump(40);
        require(polled_count == 1 && polled_latest == 9999, "Polling replayed backlog or lost latest frame");
        polled->publish(10000); polled->cancel();
        std::thread late_polled_producer([&] { for (int i = 0; i < 10000; ++i) polled->publish(i); });
        late_polled_producer.join(); pump(40);
        require(polled_count == 1, "Polling delivered after cancellation");
        QObject timer_owner;
        {
            auto replaced = std::make_shared<LatestDelivery<int>>(&timer_owner, [](int) {});
            replaced->start_polling();
        }
        pump(40);
        require(timer_owner.findChildren<QTimer *>().isEmpty(), "Replaced delivery left an active polling timer");
        for (float ratio : {0.125f,1.0f,8.0f}) {
            for (float yaw : {0.0f,0.785398f,1.570796f}) {
                const auto step = SpectrogramGraph::camera_translation(yaw,1,0,ratio,2);
                const QVector3D world(step.x()*std::min(1.0f,ratio),step.y()/2,
                                      step.z()*std::min(1.0f,1.0f/ratio));
                require(std::abs(world.length()-1.0f) < 1e-5, "Camera speed depends on rotation or graph aspect ratio");
            }
        }
        LineChartView dense_chart; dense_chart.resize(400,300); dense_chart.show(); pump(30);
        std::vector<float> dense(10000,0); dense[5051]=42; dense[5052]=-17;
        dense_chart.update(dense); pump(40);
        const auto rendered=static_cast<QLineSeries *>(dense_chart.chart()->series().front())->points();
        require(rendered.size()<1000,"Chart paints more points than visible pixels require");
        const auto [low,high]=std::minmax_element(rendered.begin(),rendered.end(),[](const auto &a,const auto &b){return a.y()<b.y();});
        require(low->y()==-17 && high->y()==42,"Chart decimation lost a short spike");
        dense_chart.hide();
        const auto legacy_path=temp.filePath("legacy.json");
        QFile legacy_file(legacy_path); require(legacy_file.open(QIODevice::WriteOnly),"Cannot create legacy config");
        legacy_file.write(R"({"ui":{"show_surface":false,"show_samples":true,"show_samples_after_window_function":true,"surface_side_panel_ratio":0.4}})"); legacy_file.close();
        FileSaveApplicationConfig legacy_config; ConfigManager legacy_manager(legacy_path);
        require(legacy_manager.load(legacy_config) && !legacy_config.ui.show_spectrogram && legacy_config.ui.show_window_signal &&
            legacy_config.ui.show_windowed_signal && std::abs(legacy_config.ui.spectrogram_side_panel_ratio-0.4)<1e-6,
            "Legacy presentation keys not migrated");
        ConfigManager manager(temp.filePath("nested/config.json"));
        FileSaveApplicationConfig saved;
        saved.wled_ar_server.port = 11989;
        saved.core.spectrum_channel = 1; saved.core.spectrum_merge = SpectrumMerge::Maximum;
        saved.ui.spectrogram_side_panel_ratio = 0.4f;
        saved.ui.camera_controls["forward"] = QJsonArray{CameraControls::binding(23,Qt::Key_I), CameraControls::binding(37,Qt::Key_K)};
        saved.ui.camera = {{"horizontal", 35}, {"vertical", 60}, {"zoom", 200}, {"target", QJsonArray{0.2,-0.3,0.4}}};
        require(manager.save(saved), "Nested configuration save failed");
        FileSaveApplicationConfig loaded;
        require(manager.load(loaded) && loaded.wled_ar_server.port == 11989, "Configuration roundtrip failed");
        require(std::abs(loaded.ui.spectrogram_side_panel_ratio - 0.4f) < 1e-6, "Spectrogram split ratio not preserved by config");
        require(loaded.core.spectrum_channel == 1 && loaded.core.spectrum_merge == SpectrumMerge::Maximum, "Stereo settings did not persist");
        require(loaded.ui.camera_controls == saved.ui.camera_controls, "Camera key bindings did not persist");
        const auto default_forward = CameraControls::defaults()["forward"].toArray()[0].toObject();
        for (int key : {int(Qt::Key_W),0x0426,int(Qt::Key_Z)}) {
            QKeyEvent physical(QEvent::KeyPress,key,Qt::NoModifier,17,0,0);
            require(CameraControls::matches(default_forward,physical), "Physical W changes with keyboard layout");
        }
        QKeyEvent russian(QEvent::KeyPress,0x0426,Qt::NoModifier);
        require(CameraControls::matches(default_forward,russian), "Russian fallback binding failed");
        QKeyEvent right_ctrl(QEvent::KeyPress,Qt::Key_Control,Qt::ControlModifier,0x11d,0,0);
        require(CameraControls::matches(CameraControls::defaults()["down"].toArray()[0].toObject(),right_ctrl), "Right Ctrl not recognized");
        require(loaded.ui.camera == saved.ui.camera, "Camera position not preserved by config");
        SpectrogramGraph camera_graph;
        camera_graph.set_camera_state(saved.ui.camera);
        const auto staged = camera_graph.camera_state();
        require(staged["horizontal"] == saved.ui.camera["horizontal"] && staged["zoom"] == saved.ui.camera["zoom"] &&
                std::abs(staged["target"].toArray()[0].toDouble() - 0.2) < 1e-6,
                "Camera state lost before lazy graph initialization");
        camera_graph.set_camera_state({{"horizontal",999}, {"vertical",-500}, {"zoom",-1}, {"target",QJsonArray{99,-99,0.5}}});
        const auto bounded = camera_graph.camera_state();
        require(bounded["horizontal"].toDouble() == 180 && bounded["vertical"].toDouble() == 0 &&
                bounded["zoom"].toDouble() == 10 && bounded["target"].toArray() == QJsonArray{1,-1,0.5}, "Invalid camera position not bounded");
        camera_graph.set_camera_state({{"zoom",1500}});
        require(camera_graph.camera_state()["zoom"].toDouble() == 1500, "Close camera position still limited to 500");
        QFile invalid(manager.getConfigPath());
        require(invalid.open(QIODevice::WriteOnly), "Cannot write invalid config");
        invalid.write(R"({"wled_ar_server":{"port":70000}})"); invalid.close();
        require(manager.load(loaded) && loaded.wled_ar_server.port == 11988, "Invalid UDP port accepted");

        Spectrogram history(5);
        history.push({1, 10}); history.push({2, 20});
        require(history.set_history_size(8), "Spectrogram resize failed");
        require(history.filled_count() == 2 && history.get_channel_history(0) == std::vector<float>({0,0,0,0,0,0,1,2}), "Partial history lost on grow");
        history.push({3,30}); history.set_history_size(2);
        require(history.get_channel_history(0) == std::vector<float>({2,3}), "Newest history lost on shrink");
        history.push({4,40});
        require(history.get_channel_history(0) == std::vector<float>({3,4}), "History write position incorrect");

        std::vector<float> wave(4096);
        constexpr size_t bin = 64;
        for (size_t i = 0; i < wave.size(); ++i) wave[i] = std::sin(2.0 * 3.141592653589793 * bin * i / wave.size());
        for (auto type : {FFT::FFT2R, FFT::FFT4R, FFT::FHT2R, FFT::FHT4R}) {
            FFT transform;
            require(transform.init(4096, type) == FFT::SUCCESS, "FFT initialization failed");
            std::vector<float> spectrum(2048);
            require(transform.run(wave, spectrum) == FFT::SUCCESS, "FFT execution failed");
            require(std::abs(spectrum[bin] - 0.5f) < 0.01f, "FFT magnitude normalization incorrect");
            auto dc = std::vector<float>(4096, 1.0f);
            require(transform.run(dc, spectrum) == FFT::SUCCESS && std::abs(spectrum[0] - 1.0f) < 0.01f, "FFT DC magnitude incorrect");
        }
        for (auto type : {FFT::FFT2R, FFT::FFT4R}) {
            FFT transform;
            std::vector<float> input(64), spectrum(32);
            for (size_t i = 0; i < input.size(); ++i) input[i] = std::sin(2.0 * 3.141592653589793 * 8 * i / input.size());
            require(transform.init(64, type) == FFT::SUCCESS && transform.run(input, spectrum) == FFT::SUCCESS && std::abs(spectrum[8] - 0.5f) < 0.01f, "FFT shared tables fail with smaller input");
            require(transform.init(63, type) == FFT::INCORRECT_CONFIGS, "Invalid FFT size accepted");
        }
        Analyzer analyzer;
        analyzer.update_sample_rate(48000);
        std::mutex mutex;
        std::condition_variable ready;
        bool done = false;
        std::vector<float> fft, mel;
        analyzer.set_update_handler([&] {
            std::lock_guard lock(mutex);
            fft = analyzer.amplitudes.left; mel = analyzer.amplitudes_test.left;
            done = true; ready.notify_one();
        });
        for (int iteration = 0; iteration < 10; ++iteration) {
            { std::lock_guard lock(mutex); done = false; }
            analyzer.start();
            analyzer.add_samples(wave, wave);
            std::unique_lock lock(mutex);
            const bool received = ready.wait_for(lock, std::chrono::seconds(2), [&] { return done; });
            lock.unlock(); analyzer.stop(); analyzer.stop();
            require(received, "FFT worker did not produce data");
            require(fft.size() == 2048 && mel.size() == 50, "Spectrum dimensions incorrect");
            require(std::distance(fft.begin(), std::max_element(fft.begin(), fft.end())) == bin, "Sine FFT peak is incorrect");
            require(std::all_of(mel.begin(), mel.end(), [](float v) { return std::isfinite(v) && v >= 0; }), "Invalid Mel values");
        }
        { LineChartView chart; chart.set_auto_resizing(true); chart.update({}); chart.update(wave); }

        { FreqChartView chart; chart.update_freq_step(10); chart.update(std::vector<float>(100, 0));
          const auto axes = chart.chart()->axes(Qt::Horizontal, chart.chart()->series().first());
          require(axes.size() == 1 && static_cast<QValueAxis *>(axes.first())->max() == 1000, "Frequency axis does not follow data size"); }
        {
            FreqChartView chart;chart.resize(320,240);chart.set_range(0,1);chart.update_freq_step(10);chart.show();
            std::vector<float> narrow(2048,0);narrow[1377]=0.8f;chart.update(narrow);pump(50);
            auto *series=qobject_cast<QLineSeries *>(chart.chart()->series().first());
            require(series,"FFT spectrum must use a batched series");
            const auto initial_count=series->count();
            auto has_peak=[&] { const auto points=series->points();return std::any_of(points.begin(),points.end(),[](auto point) { return point.x()==13770 && std::abs(point.y()-0.8)<1e-6; }); };
            require(has_peak() && initial_count<2048,"Pixel aggregation lost narrow FFT peak or its Hz position");
            chart.resize(1100,600);pump(50);
            require(has_peak() && series->count()>initial_count,"Resize did not rebuild spectrum resolution");
            chart.update_freq_step(20);pump(50);
            const auto rescaled=series->points();
            require(std::any_of(rescaled.begin(),rescaled.end(),[](auto point) { return point.x()==27540 && std::abs(point.y()-0.8)<1e-6; }),"Sample-rate change retained stale frequency positions");
            chart.update({});pump(50);require(series->count()==0,"Empty FFT retained stale peaks");
        }
        auto wait_status = [&](const char *component, pipeline::State expected) {
            for(int i=0;i<200 && app.pipeline().current_status(component).state!=expected;++i) pump(5);
            require(app.pipeline().current_status(component).state==expected,"Engine did not reach expected state");
        };
        auto feed = [&] { pipeline::AudioFrame frame; frame.sample_rate=48000; frame.left=wave;frame.right=wave;app.pipeline().feed(frame); };
        int updates = 0;
        QObject::connect(&app, &CoreApplication::amplitudes_ready, &app, [&](const auto &) { ++updates; });
        for (int i = 0; i < 10; ++i) {
            app.start_analyzer(); app.start_analyzer(); wait_status("Analyzer",pipeline::State::Running);
            feed();
            pump();
            app.stop_analyzer(); app.stop_analyzer();
            wait_status("Analyzer",pipeline::State::Stopped);
            const auto before = updates;
            pump(5);
            require(updates == before, "Stopped analyzer delivered stale data");
        }
        require(updates > 0, "No GUI spectrum delivery");
        app.start_analyzer(); wait_status("Analyzer",pipeline::State::Running);
        const auto before_stall = updates;
        std::thread audio_producer([&] {
            for (int i = 0; i < 30; ++i) {
                feed();
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        audio_producer.join();
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Drain remaining audio hops without running the GUI.
        pump(5);
        require(updates == before_stall + 1, "Analyzer replayed stale spectra after a GUI stall");
        app.stop_analyzer(); wait_status("Analyzer",pipeline::State::Stopped);
        app.start_analyzer(); wait_status("Analyzer",pipeline::State::Running);
        feed();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        app.stop_analyzer(); wait_status("Analyzer",pipeline::State::Stopped); app.start_analyzer(); wait_status("Analyzer",pipeline::State::Running);
        const auto before_restart = updates;
        pump(10);
        require(updates == before_restart, "Old generation reached a restarted analyzer");
        app.stop_analyzer();
        for (int i = 0; i < 3; ++i) {
            app.start_loopback(); pump(); app.stop_loopback(); app.stop_loopback();
        }
        app._ctx.file_configs.wled_ar_server.enabled = true;
        for (int i = 0; i < 5; ++i) {
            app.start_wled_audio_reactive_server();
            app.restart_wled_audio_reactive_server();
            wait_status("WLED",pipeline::State::Running);
            app.stop_wled_audio_reactive_server(); app.stop_wled_audio_reactive_server();
            wait_status("WLED",pipeline::State::Stopped);
        }
        app._ctx.file_configs.wled_ar_server.enabled = false;
        app.restart_wled_audio_reactive_server();
        wait_status("WLED",pipeline::State::Stopped);

        AggregatorServer server;
        // Registration correctness is covered here; the dedicated network test
        // verifies registration deadlines. Do not race a 20 ms timeout against
        // unrelated GUI/DSP work on loaded CI machines.
        require(server.start(0, 1000), "Aggregator start failed");
        TcpBDSPSocket client;
        registration_request_packet_t registration{};
        registration.uuid = QUuid::createUuid().toBytes();
        registration.component_type = ComponentType::data_source;
        registration.session_token = 1234;
        bool registered = false;
        QObject::connect(&client, &TcpBDSPSocket::on_got_packet, &app, [&](auto &packet, auto *) {
            if (packet.packet_id == static_cast<uint8_t>(PacketType::component_info_update)) registered = true;
        });
        for (int i = 0; i < 3; ++i) {
            registered = false;
            client.connectToHost(QHostAddress::LocalHost, server.serverPort());
            require(client.waitForConnected(1000), "Aggregator connect failed");
            client.send_data(static_cast<uint8_t>(PacketType::component_registration), reinterpret_cast<uint8_t *>(&registration), sizeof(registration));
            for (int attempt = 0; !registered && attempt < 200; ++attempt) pump(5);
            require(registered, "Aggregator registration/restoration failed");
            client.abort(); pump(10);
        }
        TcpBDSPSocket transient;
        transient.connectToHost(QHostAddress::LocalHost, server.serverPort());
        require(transient.waitForConnected(1000), "Transient connect failed");
        pump(5); transient.abort(); pump(30);
        server.stop(); server.stop();
        qInfo("Runtime regressions passed");
    } catch (const std::exception &error) {
        qCritical("Regression failed: %s", error.what());
        return 1;
    }
    return 0;
}
