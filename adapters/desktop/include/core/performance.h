#pragma once
#include <core/telemetry.h>
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
    CallbackSnapshot, WledPublish, WindowResize, GuiEvent, SpectrogramHandoff, SpectrogramMesh, SpectrogramCreases, SpectrogramUpload, SpectrogramSurface, SpectrogramWalls, SpectrogramLabels, SpectrogramAtlas, SpectrogramOverlay, SpectrogramGpu, SpectrogramSchedule, SpectrogramFrameGap, SpectrogramShadow,
    QuickSurfaceSync, QuickSurfaceFeedbackBuild, QuickSurfaceFeedback,
    NumericQueryWait, NumericQueryHold, NumericQueryCopy,
    NumericRenderViews, NumericRenderTimer, NumericFrameReceive, NumericHistoryReceive, NumericSnapshotReceive,
    NumericClockTick, NumericPreviewTick, NumericPreviewProduce,
    Quick2dSync, Quick2dRender, Quick2dFrame, Quick2dPublish,
    Quick2dCompose, Quick2dPaint, QtBackingStoreSync, PresenterSnapshot, CaptureStatus, QuickSurfaceOverlayPaint, Quick2dGeometry, NumericPreviewWake, NumericPreviewTimerWait,
    CaptureCallback, NumericCaptureLock, NumericCaptureWork, NumericInputProcess, NumericSelectedOutput,
    GuiDeliveryDrain, EditorNavigation, ProjectSaveCapture, ProjectSaveWrite, Count };
inline constexpr std::array<const char *,static_cast<size_t>(Stage::Count)> names{
    "capture.decode","capture.dispatch","audio.feed","audio.feed_lock","dsp.window_snapshot",
    "dsp.stereo_fft","dsp.stereo_mel","pipeline.graph","history.push","gui.publish_spectrum",
    "dsp.work","dsp.frame_gap","wled.queue","wled.send","history.snapshot",
    "gui.spectrum_queue","gui.spectrum_consume","gui.audio_queue","gui.audio_consume",
    "chart.data","chart.geometry","chart.paint","editor.preview","editor.paint","gui.heartbeat","spectrogram.data","spectrogram.geometry","spectrogram.render","camera.move","capture.discovery",
    "pipeline.callback_snapshot","wled.publish","window.resize","gui.event", "spectrogram.handoff", "spectrogram.mesh_normals", "spectrogram.creases", "spectrogram.upload", "spectrogram.surface_edges", "spectrogram.walls", "spectrogram.labels", "spectrogram.atlas", "spectrogram.overlay", "spectrogram.gpu", "spectrogram.schedule", "spectrogram.frame_gap", "spectrogram.shadow",
    "quick.surface_sync", "quick.surface_feedback_build", "quick.surface_feedback",
    "numeric.query_wait", "numeric.query_hold", "numeric.query_copy",
    "numeric.render_views", "numeric.render_timer", "numeric.frame_receive", "numeric.history_receive", "numeric.snapshot_receive",
    "numeric.clock_tick", "numeric.preview_tick", "numeric.preview_produce",
    "quick2d.sync", "quick2d.render", "quick2d.frame", "quick2d.publish_to_sync",
    "quick2d.widget_paint", "quick2d.vector_paint", "qt.window_update", "gui.presenter_snapshot", "gui.capture_status", "quick.surface_overlay_paint", "quick2d.geometry", "numeric.preview_wake", "numeric.preview_timer_wait",
    "capture.callback", "numeric.capture_lock", "numeric.capture_work", "numeric.input_process", "numeric.selected_output",
    "gui.delivery_drain", "editor.navigation", "project.save_capture", "project.save_write"};
inline constexpr size_t histogram_bins=512, event_capacity=65536;
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
#if BARELIEF_DEV_TELEMETRY
inline std::atomic<bool> enabled{false};
#else
struct DisabledFlag {
    constexpr bool load(std::memory_order = std::memory_order_relaxed) const noexcept{return false;}
    constexpr operator bool() const noexcept{return false;}
    void operator=(bool) noexcept{}
};
inline DisabledFlag enabled;
#endif
inline std::atomic<uint64_t> event_count{0};
// GUI-thread-only producer; exported on that same thread after measurement.
struct SlowGuiEvent { uint64_t duration; int type; std::array<char, 64> receiver; };
inline std::array<SlowGuiEvent, 16> slow_gui_events{};
inline size_t slow_gui_event_count = 0;
inline uint64_t now() noexcept { return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
inline void record_counter(Stage stage,uint64_t duration) noexcept {
    auto &counter=counters[static_cast<size_t>(stage)];
    counter.count.fetch_add(1,std::memory_order_relaxed);counter.total.fetch_add(duration,std::memory_order_relaxed);
    auto previous=counter.maximum.load(std::memory_order_relaxed);
    while(duration>previous && !counter.maximum.compare_exchange_weak(previous,duration,std::memory_order_relaxed)) {}
    counter.histogram[std::min<size_t>(duration/bin_ns,histogram_bins-1)].fetch_add(1,std::memory_order_relaxed);
}
inline void record_event(Stage stage,uint64_t start,uint64_t duration) noexcept {
    const auto index=event_count.fetch_add(1,std::memory_order_relaxed);
    if(index>=event_capacity) return; // Never grow or overwrite live trace storage.
    auto &event=events[index];event.stage=stage;event.start=start;event.duration=duration;
    thread_local const auto thread=static_cast<uint32_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    event.thread=thread;event.ready.store(true,std::memory_order_release);
}
inline void record(Stage stage,uint64_t start,uint64_t duration) noexcept {
#if BARELIEF_DEV_TELEMETRY
    telemetry::metric(names[static_cast<size_t>(stage)],double(duration)/1e6);
    const bool bounded=enabled.load(std::memory_order_relaxed);
    if (!bounded && !telemetry::enabled()) return;
    record_counter(stage,duration);
    if(bounded)record_event(stage,start,duration);
#else
    (void)stage;(void)start;(void)duration;
#endif
}
inline void duration(Stage stage,uint64_t value) noexcept { record(stage,now()-value,value); }
#if BARELIEF_DEV_TELEMETRY
inline const auto stage_sources=[] {
    std::array<___tracy_source_location_data,static_cast<size_t>(Stage::Count)> result{};
    for(size_t i=0;i<result.size();++i)result[i]={names[i],names[i],__FILE__,0,0};
    return result;
}();
class Scope {
    telemetry::Zone zone_;
    Stage _stage; uint64_t _start;
public:
    explicit Scope(Stage stage) noexcept : zone_(stage_sources[static_cast<size_t>(stage)]),
        _stage(stage),_start((enabled.load(std::memory_order_relaxed) || telemetry::enabled()) ? now() : 0) {}
    ~Scope() { if(_start) record(_stage,_start,now()-_start); }
};
#else
struct Scope { explicit Scope(Stage) noexcept{} };
#endif
}
