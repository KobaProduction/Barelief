#include "pipeline/audio_pipeline.h"
#include <QCoreApplication>
#include <QDebug>
#include <chrono>
#include <thread>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <miniaudio.h>
int main(int argc, char **argv) {
    qputenv("QT_FORCE_STDERR_LOGGING", "1");
    QCoreApplication app(argc, argv);
    const auto duration = argc > 1 ? std::stoi(argv[1]) : 10000;
    bool mono_test = argc > 2 && QString::fromUtf8(argv[2]) == "mono-tone";
    ma_device playback{}; bool playing = false;
    if (mono_test || (argc > 2 && QString::fromUtf8(argv[2]) == "tone")) {
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.format = ma_format_f32; config.playback.channels = 2;
        config.sampleRate = mono_test ? 44100 : 48000;
        config.pUserData = &mono_test;
        config.dataCallback = [](ma_device *device, void *output, const void *, ma_uint32 count) {
            static uint64_t offset = 0;
            auto *pcm = static_cast<float *>(output);
            for (ma_uint32 i = 0; i < count; ++i,++offset) {
                const auto signal = float(0.0001 * std::sin(2*3.141592653589793*750*
                    (offset%device->sampleRate)/device->sampleRate));
                pcm[i*2] = signal; pcm[i*2+1] = *static_cast<const bool *>(device->pUserData) ? signal : -signal;
            }
        };
        playing = ma_device_init(nullptr,&config,&playback) == MA_SUCCESS;
        if (!playing || ma_device_start(&playback) != MA_SUCCESS) return 2;
    }
    pipeline::AudioPipeline engine;
    if (argc > 2 && QString::fromUtf8(argv[2]) == "microphone") {
        QString error; if (!engine.apply_graph(pipeline::GraphProgram::defaults(0, SpectrumMerge::Energy, 1), error)) return 2;
    }
    std::atomic<uint64_t> frames{0}, nonzero{0}, mono_mismatches{0}; std::mutex mutex; std::vector<uint64_t> latency;
    latency.reserve(65536);
    const auto sample_stride = std::max(1, duration / 1000 * 200 / 65536);
    uint64_t delivered = 0;
    engine.set_audio_handler([&](auto frame) {
        frames += frame.left.size();
        if (mono_test && frame.left != frame.right) ++mono_mismatches;
        for (auto sample : frame.left) if (std::abs(sample) > 0.00001f) ++nonzero;
        const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
        std::lock_guard lock(mutex);
        if (++delivered % sample_stride == 0 && latency.size() < 65536 && now >= frame.timestamp_ns) latency.push_back(now - frame.timestamp_ns);
    });
    if (argc > 2 && QString::fromUtf8(argv[2]) == "lifecycle") {
        for (int iteration = 0; iteration < 8; ++iteration) {
            engine.start_capture();
            std::this_thread::sleep_for(std::chrono::milliseconds(iteration % 3 * 10));
            const auto begin = std::chrono::steady_clock::now();
            engine.stop_capture();
            const auto elapsed = std::chrono::steady_clock::now() - begin;
            const auto stopped_frames = frames.load();
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            if (elapsed > std::chrono::seconds(2) || frames != stopped_frames ||
                engine.current_status("Capture").state != pipeline::State::Stopped) {
                qCritical("Capture cancellation/restart failed at iteration %d", iteration); return 1;
            }
        }
        qInfo("Capture opening/retry cancellation and no delivery after Stop verified"); return 0;
    }
    engine.start_analyzer(); engine.start_capture();
    const auto begin = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - begin < std::chrono::milliseconds(duration)) {
        // Deliberately do not pump the GUI/application event queue.
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        const auto d = engine.capture_diagnostics();
        qInfo() << "Capture" << d.backend << d.device << d.sample_rate << "queued" << d.buffered_frames
                << "dropped" << d.dropped_frames << "gaps" << d.discontinuities << "DSP" << engine.processed_frames();
    }
    const auto processed = engine.processed_frames();
    const auto diagnostics = engine.capture_diagnostics();
    const auto stop = std::chrono::steady_clock::now(); engine.stop_capture(); engine.stop_analyzer();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-stop).count();
    std::sort(latency.begin(), latency.end());
    if (!latency.empty()) for (const auto percent : {50,95,99}) qInfo() << "latency p" << percent << latency[(latency.size()-1)*percent/100]/1000000.0 << "ms";
    qInfo() << "PCM frames" << frames.load() << "nonzero samples" << nonzero.load() << "stop ms" << stop_ms;
    if (playing) ma_device_uninit(&playback);
    if (mono_mismatches) qCritical("Mono capture did not duplicate both channels");
    const auto minimum_processed = uint64_t(std::max(0, duration / 1000 - 1)) * 45;
    if (processed < minimum_processed) qCritical() << "Analysis stalled:" << processed << "expected at least" << minimum_processed;
    if (frames.load() && !diagnostics.discontinuities) qCritical("First audio frame did not mark the new capture timeline");
    return frames.load() && !mono_mismatches && diagnostics.discontinuities && processed >= minimum_processed && stop_ms < 2000 && (!playing || nonzero.load()) ? 0 : 1;
}
