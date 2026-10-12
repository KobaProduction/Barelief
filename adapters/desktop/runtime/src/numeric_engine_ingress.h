#pragma once
#include "numeric_engine_delivery.h"

namespace pipeline::numeric_engine_detail {
// Owns capture resources and source time advancement. Prepared execution is
// borrowed through the shared boundary; this is never an alternate DSP executor.
struct SourceIngress {
    ExecutionBoundary& execution;
    DeliveryHub& delivery;
    NumericEngine& owner;
    std::mutex& mutex;
    EngineSnapshot& state;
    dataflow_runtime::GraphExecutor& executor;
    struct CaptureInput {
        domain::NodeId node=-1;
        domain::PortId port=0;
        std::size_t lane=0,source_lanes=1;
        AudioSourceKind source=AudioSourceKind::SystemOutput;
        QJsonObject configuration;
        domain::OwnedProcessingContract owned;
        NumericContract contract;
        std::array<PlaneLayout,1> planes{};
        std::vector<std::byte> bytes;
        std::size_t block=0,filled=0;
        std::int64_t ticks=0;
        std::uint64_t sequence=0,generation=0;
        std::uint64_t producer_epoch=0;
        bool gap=true;
        bool admission_failed=false;
        std::uint64_t next_sample=0,last_frame_ns=0,clock_origin_ns=0,clock_base_sample=0;
        bool synthetic=false;
    };
    std::vector<CaptureInput> inputs;
    std::map<domain::NodeId,QString> capture_devices;
    struct ActiveSource {AudioSourceKind kind;QJsonObject configuration;std::unique_ptr<IAudioSource> source;};
    std::map<domain::NodeId,ActiveSource> sources;
    std::map<domain::NodeId,std::uint64_t> source_tokens;
    std::map<domain::NodeId,ComponentStatus> source_states;
    struct ClockInput {
        domain::NodeId node=-1;std::uint32_t rate=0;std::size_t block=0;
        std::uint64_t origin_ns=0,next_sample=0,sequence=0,producer_epoch=0;bool failed=false;
        std::uint64_t lag_samples=0,max_lag_samples=0,catchup_limits=0;
    };
    std::map<domain::NodeId,ClockInput> clocks;
    struct ExternalInput {
        domain::OwnedProcessingContract contract;
        Ratio cadence{1,120};std::uint32_t rate=0;std::size_t block=0;
        FrameStamp stamp;std::uint64_t origin_ns=0,ordinal=0,zero_frames=0;
        bool missing=true,initial=true,failed=false;
    };
    std::map<domain::NodeId,ExternalInput> external_inputs;
    AudioSourceFactory factory;
    bool native_capture_backend=false,capture_requested=false;
    std::size_t working_capture_bytes=0;
    std::uint64_t capture_fence=1;
    QTimer clock_timer;
    std::function<void()> snapshot_changed;
    SourceIngress(ExecutionBoundary&,DeliveryHub&,AudioSourceFactory);
    struct Staged {
        std::vector<CaptureInput> inputs_candidate;
        decltype(clocks) clocks_candidate;
        decltype(external_inputs) external_inputs_candidate;
        std::size_t capture_bytes=0;
    };
    static bool physical_supported(const domain::ResolvedProcessingNode&) noexcept;
    static Status capture_reservation(const domain::NumericProcessingGraph&,std::size_t&) noexcept;
    Status stage(const domain::NumericProcessingGraph&,const domain::ProcessingAdmissionLimits&,Staged&);
    void commit(Staged&&);
    void clear();
    Status process(domain::NodeId,const DataFrameView&,domain::PortId port=0);
    Status emit_input(CaptureInput& input);
    void capture(domain::NodeId node,std::uint64_t fence,const AudioFrame& frame);
    void note_external(domain::NodeId node,const DataFrameView& frame,Status status);
    void advance_clocks();
    void stop_sources();
    void update_capture_status();
    void source_status(domain::NodeId node,std::uint64_t fence,bool running,const QString& error);
    void reconcile_sources();
    void reproject_failed_sources();
};
}
