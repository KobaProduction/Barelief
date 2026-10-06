#include "synthetic_capture.h"
#include "pipeline/audio_pipeline.h"
#include <QCoreApplication>
#include <QUdpSocket>
#include <thread>
#include <chrono>
#include <cmath>
#include <stdexcept>
#include <QDebug>
#include <condition_variable>
#include <cstring>
#include <limits>
#include "process_memory.h"
static void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
class FailingTask final : public AbstractTask {
    uint64_t _before_run_task() override { return 42; }
    uint64_t _task() override { return 0; }
};
class RetryTask final : public AbstractTask {
    uint64_t _task() override { while (_running.load()) _sleep(5000); return 0; }
};
int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    try {
        std::mutex status_mutex;
        std::condition_variable changed;
        bool failure_seen = false;
        FailingTask failing;
        failing.set_lifecycle_handler([&](bool ready, uint64_t result, const auto &) {
            std::lock_guard lock(status_mutex); failure_seen = !ready && result == 42; changed.notify_one();
        });
        failing.start();
        { std::unique_lock lock(status_mutex); require(changed.wait_for(lock,std::chrono::seconds(1),[&]{return failure_seen;}), "Initialization failure was not reported"); }
        failing.stop();
        RetryTask retry; retry.start(); std::this_thread::sleep_for(std::chrono::milliseconds(10));
        auto stop_start = std::chrono::steady_clock::now(); retry.stop();
        require(std::chrono::steady_clock::now()-stop_start < std::chrono::milliseconds(250), "Retry wait cannot be cancelled");
        pipeline::AudioPipeline engine(test_fixture::synthetic_capture_factory(false));
        QUdpSocket receiver;
        require(receiver.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)), "UDP receiver bind failed");
        engine.start_wled(QHostAddress::LocalHost, receiver.localPort(), QHostAddress::LocalHost);
        engine.start_analyzer();
        pipeline::AudioFrame frame;
        frame.sample_rate = 48000; frame.left.resize(4096); frame.right.resize(4096);
        for (size_t i = 0; i < frame.left.size(); ++i) frame.left[i] = frame.right[i] = std::sin(2 * 3.141592653589793 * 64 * i / 4096);
        // Deliberately never process the main/UI event loop while audio runs.
        for (int i = 0; i < 50; ++i) {
            engine.feed(frame);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        require(engine.processed_frames() >= 15, "DSP stopped with the GUI event loop blocked");
        require(engine.history_count() >= 15, "History depends on GUI delivery");
        require(receiver.hasPendingDatagrams() || receiver.waitForReadyRead(1000), "Network output depends on GUI delivery");
        engine.latest_history();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto snapshot=engine.latest_history();
        require(snapshot && !snapshot->values.empty(),"History snapshot depends on GUI event loop");
        engine.stop_analyzer(); engine.stop_analyzer();
        bool found_frequency = false;
        while (receiver.hasPendingDatagrams()) {
            QByteArray bytes; bytes.resize(receiver.pendingDatagramSize());
            receiver.readDatagram(bytes.data(),bytes.size());
            require(bytes.size() == 44, "WLED wire size incorrect");
            wled_audio_reactive::AudioSyncPacket packet{}; std::memcpy(&packet,bytes.constData(),sizeof(packet));
            require(std::memcmp(packet.header,"00002",6)==0, "WLED protocol header incorrect");
            if (std::abs(packet.FFT_MajorPeak-750) < 1) found_frequency = true;
            require(std::isfinite(packet.sampleRaw) && packet.sampleRaw >= 0 && packet.sampleRaw <= 255, "WLED level outside protocol range");
        }
        require(found_frequency,"WLED peak is not in Hz");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        bool silence_seen=false;
        receiver.waitForReadyRead(200);
        while (receiver.hasPendingDatagrams()) {
            QByteArray bytes(44,0); receiver.readDatagram(bytes.data(),bytes.size());
            wled_audio_reactive::AudioSyncPacket packet{}; std::memcpy(&packet,bytes.constData(),sizeof(packet));
            if (packet.sampleRaw == 0 && packet.FFT_MajorPeak == 0) silence_seen=true;
        }
        require(silence_seen,"WLED kept stale audio after capture stopped");
        const auto sanitized = WLEDAudioReactiveServer::make_packet({std::numeric_limits<float>::quiet_NaN(),100,-1}, std::numeric_limits<float>::infinity(),-1,255);
        require(sanitized.fftResult[0]==0 && sanitized.fftResult[1]==255 && sanitized.fftResult[2]==0 && sanitized.FFT_MajorPeak==0 && sanitized.FFT_Magnitude==0, "WLED invalid values not sanitized");
        require(WLEDAudioReactiveServer::make_packet({},23000,1,0).FFT_MajorPeak==23000,"WLED peak incorrectly assumes 44.1kHz input");
        const auto count = engine.processed_frames();
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        require(engine.processed_frames() == count, "Stopped analyzer kept processing");
        require(engine.current_status("Analyzer").state == pipeline::State::Stopped, "Stopped state incorrect");
        auto revision = engine.current_status("Analyzer").revision;
        engine.start_analyzer();
        for (int i=0; i<100 && engine.current_status("Analyzer").state != pipeline::State::Running; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        require(engine.current_status("Analyzer").state == pipeline::State::Running && engine.current_status("Analyzer").revision > revision, "Analyzer did not report actual readiness");
        engine.stop_analyzer();
        for (int scenario = 0; scenario < 3; ++scenario) {
            pipeline::ViewFrame result;
            bool received = false;
            engine.set_spectrum_handler([&](auto value) { if(!value.spectrum.sequence) return; std::lock_guard lock(status_mutex); result = std::move(value); received = result.spectrum.sequence >= 5; changed.notify_one(); });
            auto stereo = frame;
            if (scenario == 0) std::fill(stereo.right.begin(), stereo.right.end(), 0);
            if (scenario == 1) std::fill(stereo.left.begin(), stereo.left.end(), 0);
            if (scenario == 2) for (auto &v : stereo.right) v = -v;
            engine.set_display_channel(scenario == 1 ? 1 : 2);
            engine.start_analyzer(); engine.feed(stereo);
            { std::unique_lock lock(status_mutex); require(changed.wait_for(lock,std::chrono::seconds(1),[&]{return received;}), "No stereo frame"); }
            engine.stop_analyzer();
            const auto left_peak = *std::max_element(result.spectrum.left.begin(),result.spectrum.left.end());
            const auto right_peak = *std::max_element(result.spectrum.right.begin(),result.spectrum.right.end());
            require((scenario != 1 || left_peak == 0) && (scenario != 0 || right_peak == 0), "Silent channel has a spectrum");
            require(*std::max_element(result.fft.begin(),result.fft.end()) > 0.1f, "Right-only or antiphase signal lost");
            require(*std::max_element(result.mel.begin(),result.mel.end()) > 0.1f, "Merged Mel lost stereo frequencies");
        }
        require(std::abs(Analyzer::merge_spectra({2},{4},SpectrumMerge::Mean)[0]-3) < 1e-6, "Mean merge incorrect");
        require(Analyzer::merge_spectra({2},{4},SpectrumMerge::Maximum)[0] == 4, "Maximum merge incorrect");
        require(std::abs(Analyzer::merge_spectra({2},{4},SpectrumMerge::Energy)[0]-std::sqrt(10.0f)) < 1e-6, "Energy merge incorrect");
        engine.set_spectrum_handler({});
        engine.start_analyzer();
        const auto begin = std::chrono::steady_clock::now();
        const auto memory_before = resident_memory();
        uint64_t worst_feed_us = 0;
        std::atomic<int64_t> worst_latency_ns{0};
        engine.set_spectrum_handler([&](auto output) {
            if(!output.spectrum.input_timestamp_ns) return;
            auto now=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
            const auto latency=now-output.spectrum.input_timestamp_ns;
            auto previous=worst_latency_ns.load(); while(latency>previous && !worst_latency_ns.compare_exchange_weak(previous,latency)) {}
        });
        #ifdef _WIN32
        HANDLE producer_timer=CreateWaitableTimerExW(nullptr,nullptr,0x00000002,TIMER_ALL_ACCESS);
        require(producer_timer,"High resolution producer timer unavailable");
#endif
        pipeline::AudioFrame paced; paced.sample_rate=48000; paced.left.resize(240);paced.right.resize(240);
        for (int i = 0; i < 200; ++i) {
            for(size_t j=0;j<240;++j) paced.left[j]=paced.right[j]=std::sin(2*3.141592653589793*750*(i*240+j)/48000);
            const auto before = std::chrono::steady_clock::now(); engine.feed(paced);
            worst_feed_us = std::max(worst_feed_us, static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-before).count()));
            const auto deadline=begin+std::chrono::milliseconds((i+1)*5);
            #ifdef _WIN32
            const auto remaining=std::chrono::duration_cast<std::chrono::nanoseconds>(deadline-std::chrono::steady_clock::now()).count();
            LARGE_INTEGER due; due.QuadPart=-std::max<int64_t>(1,remaining/100);
            require(SetWaitableTimer(producer_timer,&due,0,nullptr,nullptr,FALSE),"Producer timer failed");
            WaitForSingleObject(producer_timer,INFINITE);
#else
            std::this_thread::sleep_until(deadline);
#endif
        }
        #ifdef _WIN32
        CloseHandle(producer_timer);
#endif
        const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
        require(engine.history_count()<=400,"History grew past configured limit");
        engine.stop_analyzer();
        const auto growth = resident_memory() - memory_before;
        qInfo("Short-run working-set growth: %.3f MiB",growth/1048576.0);
        require(growth<32*1024*1024,"Unexpected memory growth in bounded pipeline");
        const auto measured_fps = engine.processed_frames()/elapsed;
        qInfo("Performance: %.1f analysis frames/s, worst feed %.3f ms", measured_fps,worst_feed_us/1000.0);
        qInfo("Worst input-to-result latency: %.3f ms",worst_latency_ns.load()/1000000.0);
        require(worst_latency_ns.load()>0 && worst_latency_ns.load()<100000000,"DSP latency exceeds 100ms");
        require(measured_fps > 55 && measured_fps < 65, "Analysis cadence is outside the 60Hz budget");
        auto burst = frame;
        std::fill(burst.left.begin(),burst.left.end(),0);
        std::fill(burst.right.begin(),burst.right.end(),0);
        for (int i=2000; i<2096; ++i) burst.right[i] = std::sin(2*3.141592653589793*8000*i/48000);
        pipeline::ViewFrame detected; bool burst_ready=false;
        engine.set_spectrum_handler([&](auto v){
            std::lock_guard lock(status_mutex);
            if (!burst_ready && v.fft.size()>=800 && *std::max_element(v.fft.begin()+580,v.fft.begin()+800)>0.005f) {
                detected=std::move(v); burst_ready=true; changed.notify_one();
            }
        });
        engine.set_display_channel(2); engine.start_analyzer();
        auto burst_begin = std::chrono::steady_clock::now(); engine.feed(burst);
        { std::unique_lock lock(status_mutex); require(changed.wait_for(lock,std::chrono::seconds(1),[&]{return burst_ready;}), "High-frequency burst was not analysed"); }
        engine.stop_analyzer();
        require(*std::max_element(detected.fft.begin()+580,detected.fft.begin()+800) > 0.005f, "2ms high-frequency burst lost");
        qInfo("2ms 8kHz burst delivery: %.3f ms (4096-sample analysis window)", std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-burst_begin).count());
        engine.set_spectrum_handler({});
        engine.stop_wled(); engine.stop_wled();
        engine.start_wled(QHostAddress("203.0.113.1"), receiver.localPort(), QHostAddress::LocalHost);
        require(engine.current_status("WLED").state == pipeline::State::Failed && !engine.wled(), "WLED bind failure reported as running");
        qInfo("Independent pipeline tests passed: %llu frames during main-loop stall", static_cast<unsigned long long>(count));
    } catch (const std::exception &error) { qCritical("Pipeline test: %s", error.what()); return 1; }
}
