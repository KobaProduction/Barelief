#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <algorithm>

// Opt-in, bounded instrumentation. DSP hooks do not allocate, format or write files.
namespace core::perf {
enum class Stage { CaptureDecode, CaptureDispatch, Feed, FeedLock, WindowSnapshot,
    FFT, StereoMel, Graph, HistoryPush, SpectrumPublish, AnalysisWork, AnalysisGap,
    WledQueue, WledSend, HistorySnapshot, GuiSpectrumQueue, GuiSpectrumConsume,
    GuiAudioQueue, GuiAudioConsume, ChartData, ChartGeometry, ChartPaint,
    NodePreview, EditorPaint, GuiHeartbeat, SpectrogramData, SpectrogramGeometry, SpectrogramRender, CameraMove, CaptureDiscovery,
    CallbackSnapshot, WledPublish, WindowResize, GuiEvent, Count };
inline constexpr std::array<const char *,static_cast<size_t>(Stage::Count)> names{
    "capture.decode","capture.dispatch","audio.feed","audio.feed_lock","dsp.window_snapshot",
    "dsp.stereo_fft","dsp.stereo_mel","pipeline.graph","history.push","gui.publish_spectrum",
    "dsp.work","dsp.frame_gap","wled.queue","wled.send","history.snapshot",
    "gui.spectrum_queue","gui.spectrum_consume","gui.audio_queue","gui.audio_consume",
    "chart.data","chart.geometry","chart.paint","editor.preview","editor.paint","gui.heartbeat","spectrogram.data","spectrogram.geometry","spectrogram.render","camera.move","capture.discovery",
    "pipeline.callback_snapshot","wled.publish","window.resize","gui.event"};
inline constexpr size_t histogram_bins=512, event_capacity=32768;
inline constexpr uint64_t bin_ns=250000;
struct Counter {
    std::atomic<uint64_t> count{0},total{0},maximum{0};
    std::array<std::atomic<uint64_t>,histogram_bins> histogram{};
};
// Read after stopping measurement. The final histogram bin also contains values
// above its nominal range, so its finite edge is not an upper latency bound.
inline uint64_t percentile_upper_ns(const Counter &counter, unsigned percentile) noexcept {
    const auto count = counter.count.load(std::memory_order_relaxed);
    if (!count) return 0;
    percentile = std::clamp(percentile, 1u, 100u);
    const auto threshold = (count / 100) * percentile + ((count % 100) * percentile + 99) / 100;
    uint64_t accumulated = 0;
    for (size_t bin = 0; bin < histogram_bins; ++bin) {
        accumulated += counter.histogram[bin].load(std::memory_order_relaxed);
        if (accumulated >= threshold)
            return bin + 1 < histogram_bins ? (bin + 1) * bin_ns : counter.maximum.load(std::memory_order_relaxed);
    }
    return counter.maximum.load(std::memory_order_relaxed);
}
struct Event { Stage stage{}; uint64_t start=0,duration=0; uint32_t thread=0; std::atomic<bool> ready{false}; };
inline std::array<Counter,static_cast<size_t>(Stage::Count)> counters{};
inline std::array<Event,event_capacity> events{};
inline std::atomic<bool> enabled{false};
inline std::atomic<uint64_t> event_count{0};
// GUI-thread-only producer; exported on that same thread after measurement.
struct SlowGuiEvent { uint64_t duration; int type; std::array<char, 64> receiver; };
inline std::array<SlowGuiEvent, 16> slow_gui_events{};
inline size_t slow_gui_event_count = 0;
inline uint64_t now() noexcept { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void record(Stage stage,uint64_t start,uint64_t duration) noexcept {
    if (!enabled.load(std::memory_order_relaxed)) return;
    auto &counter=counters[static_cast<size_t>(stage)];
    counter.count.fetch_add(1,std::memory_order_relaxed);counter.total.fetch_add(duration,std::memory_order_relaxed);
    auto previous=counter.maximum.load(std::memory_order_relaxed);
    while(duration>previous && !counter.maximum.compare_exchange_weak(previous,duration,std::memory_order_relaxed)) {}
    counter.histogram[std::min<size_t>(duration/bin_ns,histogram_bins-1)].fetch_add(1,std::memory_order_relaxed);
    const auto index=event_count.fetch_add(1,std::memory_order_relaxed);
    if(index>=event_capacity) return; // Never grow or overwrite live trace storage.
    auto &event=events[index];event.stage=stage;event.start=start;event.duration=duration;
    thread_local const auto thread=static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    event.thread=thread;event.ready.store(true,std::memory_order_release);
}
inline void duration(Stage stage,uint64_t value) noexcept { record(stage,now()-value,value); }
class Scope {
    Stage _stage; uint64_t _start;
public:
    explicit Scope(Stage stage) noexcept : _stage(stage),_start(enabled.load(std::memory_order_relaxed) ? now() : 0) {}
    ~Scope() { if(_start) record(_stage,_start,now()-_start); }
};
}
