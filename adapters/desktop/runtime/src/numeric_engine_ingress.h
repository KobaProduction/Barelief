#pragma once
#include "numeric_engine_delivery.h"
#include <dataflow_runtime/pcm_packet_queue.h>

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
        bool select_native_lanes=false;
        AudioSourceKind source=AudioSourceKind::SystemOutput;
        QJsonObject configuration;
        domain::OwnedProcessingContract owned;
        NumericContract contract;
        std::array<PlaneLayout,1> planes{};
        std::vector<std::byte> bytes;
        dataflow_runtime::PcmPacketQueue packets;
        std::vector<float> packet_scratch;
        std::uint64_t packet_sequence=0,dropped_samples=0;
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
    static bool physical_output_supported(const domain::ProcessingPort&,std::uint32_t& rate) noexcept;
    static Status capture_reservation(const domain::NumericProcessingGraph&,std::size_t&) noexcept;
    static Status reserve_capture_node(const domain::ConfiguredProcessingNode&,std::size_t&) noexcept;
    static Status reserve_capture_port(const domain::ProcessingPort&,std::size_t&) noexcept;
    Status stage(const domain::NumericProcessingGraph&,const domain::ProcessingAdmissionLimits&,Staged&);
    void stage_external_inputs(const domain::NumericProcessingGraph&,Staged&);
    Status stage_sample_clocks(const domain::NumericProcessingGraph&,Staged&);
    Status stage_unbound_generators(const domain::NumericProcessingGraph&,Staged&);
    void stage_constants(const domain::NumericProcessingGraph&,Staged&);
    Status stage_capture_inputs(const domain::NumericProcessingGraph&,const domain::ProcessingAdmissionLimits&,Staged&);
    Status stage_capture_node(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,Staged&);
    Status configure_capture_input(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,
                                   std::size_t lane,CaptureInput&);
    Status configure_capture_parameters(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,
                                        CaptureInput&);
    Status configure_audio_capture(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,
                                   CaptureInput&);
    static Status configure_test_signal(const domain::ConfiguredProcessingNode&,
                                        const domain::ResolvedProcessingNode&,CaptureInput&);
    static Status configure_capture_source(const domain::ConfiguredProcessingNode&,CaptureInput&);
    void retain_capture_state(CaptureInput&);
    void commit(Staged&&);
    void clear();
    Status process(domain::NodeId,const DataFrameView&,domain::PortId port=0);
    Status emit_input(CaptureInput& input);
    FrameStamp input_stamp(CaptureInput& input) const;
    void capture(domain::NodeId node,std::uint64_t fence,const AudioFrame& frame);
    std::uint64_t resumed_capture_epoch(domain::NodeId node,const AudioFrame& frame);
    bool capture_contract_matches(const CaptureInput& input,const AudioFrame& frame) const;
    void reject_capture(CaptureInput& input,domain::NodeId node,std::uint64_t fence,const AudioFrame& frame);
    void process_capture_input(CaptureInput& input,domain::NodeId node,std::uint64_t fence,
                               const AudioFrame& frame,std::uint64_t resumed_epoch);
    void enqueue_capture_packets(CaptureInput& input,const AudioFrame& frame,const float* samples,bool capture_gap);
    void drain_capture_packets(CaptureInput& input);
    void note_external(domain::NodeId node,const DataFrameView& frame,Status status);
    void note_available_external(domain::NodeId node,ExternalInput& input,const DataFrameView& frame);
    bool adjust_external_interval(ExternalInput& input,const FrameStamp& stamp);
    void advance_clocks();
    void advance_sample_clocks(std::uint64_t now);
    void advance_sample_clock(domain::NodeId node,ClockInput& clock,std::uint64_t now);
    void advance_missing_inputs(std::uint64_t now);
    void advance_missing_input(domain::NodeId node,ExternalInput& input,std::uint64_t now);
    bool emit_missing_frame(domain::NodeId node,ExternalInput& input);
    void advance_failed_sources(std::uint64_t now);
    void advance_failed_source(CaptureInput& input,std::uint64_t now);
    void stop_sources();
    void update_capture_status();
    void source_status(domain::NodeId node,std::uint64_t fence,bool running,const QString& error);
    void mark_source_inputs_failed(domain::NodeId node);
    void reconcile_sources();
    using NeededSources=std::map<domain::NodeId,std::pair<AudioSourceKind,QJsonObject>>;
    NeededSources needed_sources();
    void reconcile_existing_sources(const NeededSources&);
    bool reconfigure_source(decltype(sources)::iterator,const NeededSources&);
    void start_needed_sources(const NeededSources&);
    void start_source(domain::NodeId node,AudioSourceKind kind,const QJsonObject& settings);
    std::uint64_t start_source_fence(domain::NodeId node,bool available);
    void reproject_failed_sources();
};
}
