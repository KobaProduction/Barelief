#pragma once
#include "numeric_engine_execution.h"
#include <QUdpSocket>
#include <QPointer>
#include <wled/numeric_wled_output_policy.h>
#include <array>
#include <atomic>
#include <tuple>

namespace pipeline::numeric_engine_detail {
struct DeliveryArena {
    static constexpr std::size_t slot_count=128,payload_bytes=262144,metadata_bytes=16384;
    std::vector<std::byte> payload{slot_count*payload_bytes},metadata{slot_count*metadata_bytes};
    std::array<BufferSlot,slot_count> payload_slots{},metadata_slots{};
    BufferPool payload_pool{payload,payload_slots,payload_bytes};
    BufferPool metadata_pool{metadata,metadata_slots,metadata_bytes};
};
// Full history is queried on demand, separately from compact realtime rows.
// Eight bounded snapshot leases avoid enlarging all 256 delivery/preview slots.
struct HistorySnapshotArena {
    static constexpr std::size_t slot_count=8,payload_bytes=524288,metadata_bytes=16384;
    std::vector<std::byte> payload{slot_count*payload_bytes},metadata{slot_count*metadata_bytes};
    std::array<BufferSlot,slot_count> payload_slots{},metadata_slots{};
    BufferPool payload_pool{payload,payload_slots,payload_bytes};
    BufferPool metadata_pool{metadata,metadata_slots,metadata_bytes};
};
// Owns all asynchronous leases, subscriptions, cancellation identities and
// presentation deadlines. The executor only calls the synchronous callback view.
struct DeliveryHub {
    ExecutionBoundary& execution;
    NumericEngine& owner;
    std::mutex& mutex;
    EngineSnapshot& state;
    std::shared_ptr<DeliveryArena> arena=std::make_shared<DeliveryArena>(),preview_arena=std::make_shared<DeliveryArena>();
    std::shared_ptr<HistorySnapshotArena> history_snapshot_arena=std::make_shared<HistorySnapshotArena>();
    std::shared_ptr<const void> retention_arenas=std::make_shared<std::tuple<std::shared_ptr<DeliveryArena>,std::shared_ptr<DeliveryArena>,std::shared_ptr<HistorySnapshotArena>>>(arena,preview_arena,history_snapshot_arena);
    std::map<EndpointIdentity,std::shared_ptr<const NumericDeliveryContext>> contexts;
    std::map<EndpointIdentity,std::uint64_t> history_ordinals;
    std::map<EndpointIdentity,DeliveredNumericFrame> latest_matrices; // Prepared keys; shares bounded retained delivery ownership.
    std::map<EndpointIdentity,bool> delivery_gaps;
    struct WledOutput {
        QHostAddress destination,interface;quint16 port=11988;
        desktop::NumericWledOutputPolicy policy;
        std::shared_ptr<QUdpSocket> socket;
    };
    std::map<domain::NodeId,WledOutput> wled_outputs;
    NumericDeliveryMailbox<DeliveredNumericFrame,128> frames,previews;
    NumericDeliveryMailbox<DeliveredNumericHistoryRow,128> rows,preview_rows;
    std::vector<EndpointIdentity> subscriptions;
    struct Tap {
        std::shared_ptr<std::atomic_bool> active=std::make_shared<std::atomic_bool>(true);
        std::vector<EndpointIdentity> endpoints;
        std::function<void(DeliveredNumericFrame)> frame;
        std::function<void(DeliveredNumericHistoryRow)> history;
    };
    std::map<std::uint64_t,Tap> taps;
    std::uint64_t next_tap=0;
    std::uint64_t transport_frame_drops=0,transport_row_drops=0,preview_coalesced=0,preview_row_drops=0;
    std::size_t tap_queue_depth=128;
    QTimer preview_timer,wled_timer;
    bool preview_scheduled=false,preview_flushing=false;
    int preview_interval_ms=16;
    std::uint64_t preview_last_flush_ns=0,preview_timer_armed_ns=0,delivery_generation=0;
    std::function<void(DeliveredNumericFrame)> frame_handler;
    std::function<void(DeliveredNumericHistoryRow)> history_handler;
    std::shared_ptr<std::atomic_bool> frame_active=std::make_shared<std::atomic_bool>(true),history_active=std::make_shared<std::atomic_bool>(true);
    explicit DeliveryHub(ExecutionBoundary&);
    struct Staged {
        decltype(contexts) contexts_candidate;
        decltype(history_ordinals) history_ordinals_candidate;
        decltype(latest_matrices) latest_matrices_candidate;
        decltype(delivery_gaps) delivery_gaps_candidate;
        decltype(wled_outputs) wled_outputs_candidate;
    };
    Status stage(const domain::NumericProcessingGraph&,const domain::ProcessingAdmissionLimits&,Staged&);
    void stage_history_port(const std::shared_ptr<NumericDeliveryContext>&,const DataContract&,Staged&);
    void stage_node_ports(const domain::NumericProcessingGraph&,const domain::ConfiguredProcessingNode&,domain::NodeRole,Staged&);
    Status assign_wled_address(domain::NodeId,const char*,std::string_view,WledOutput&);
    Status assign_wled_parameter(domain::NodeId,const char*,const domain::ProcessingParameterView&,WledOutput&,std::uint32_t&);
    void stage_port(const domain::NumericProcessingGraph&,const domain::ConfiguredProcessingNode&,
                    const domain::ProcessingPort&,Staged&);
    Status stage_wled(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,Staged&);
    Status configure_wled_output(const domain::ConfiguredProcessingNode&,const domain::ResolvedProcessingNode&,
                                 WledOutput&,std::uint32_t& cadence);
    void commit(Staged&&);
    void clear();
    dataflow_runtime::NumericRuntimeCallbacks callbacks() noexcept;
    bool tap_selected(const EndpointIdentity& endpoint) const;
    bool selected(const EndpointIdentity& endpoint) const;
    using ContextIterator=decltype(contexts)::iterator;
    using LatestIterator=decltype(latest_matrices)::iterator;
    ContextIterator find_context(const dataflow_runtime::NumericPortFrameView& borrowed);
    void update_wled_input(const dataflow_runtime::NumericPortFrameView& borrowed);
    std::size_t count_preview_matches(const EndpointIdentity&);
    bool discard_preview(const DeliveredNumericFrame&,const EndpointIdentity&,const FrameStamp&,bool,std::size_t&,std::size_t) const;
    void commit_tap_frame(ContextIterator,LatestIterator,const dataflow_runtime::NumericPortFrameView&,DeliveredNumericFrame,Status);
    ContextIterator history_context(const dataflow_runtime::NumericHistoryRowView&);
    void record_transport_drop(ContextIterator);
    void update_tap_budget();
    void update_wled_timer();
    std::size_t queued_tap_frames(const EndpointIdentity&);
    static NumericHistoryEvent history_event(dataflow_runtime::NumericHistoryEventKind);
    bool delivery_current(std::uint64_t,const DeliveryStamp&) const;
    bool generation_current(std::uint64_t) const;
    template<class Frame> struct PendingCallback {
        std::function<void(Frame)> callback;
        std::shared_ptr<std::atomic_bool> active;
    };
    struct PreviewFrameDispatch {
        std::optional<DeliveredNumericFrame> next;
        PendingCallback<DeliveredNumericFrame> handler;
        std::uint64_t generation=0;
        bool duplicate=false;
    };
    struct PreviewRowDispatch {
        std::optional<DeliveredNumericHistoryRow> next;
        PendingCallback<DeliveredNumericHistoryRow> handler;
        std::uint64_t generation=0;
    };
    bool dispatch_preview_frame(const PreviewFrameDispatch&,QPointer<NumericEngine>);
    PreviewFrameDispatch pop_preview_frame(std::array<EndpointIdentity,128>&,std::size_t&);
    PreviewRowDispatch pop_preview_row();
    bool preview_subscribed(const NumericDeliveryContext*,const DeliveryStamp&) const;
    using FrameCallbacks=std::array<PendingCallback<DeliveredNumericFrame>,9>;
    using RowCallbacks=std::array<PendingCallback<DeliveredNumericHistoryRow>,9>;
    std::size_t select_frame_callbacks(const DeliveredNumericFrame&,FrameCallbacks&) const;
    std::size_t select_row_callbacks(const DeliveredNumericHistoryRow&,RowCallbacks&) const;
    bool dispatch_frame_callbacks(const DeliveredNumericFrame&,const FrameCallbacks&,std::size_t,std::uint64_t,QPointer<NumericEngine>);
    bool dispatch_row_callbacks(const DeliveredNumericHistoryRow&,const RowCallbacks&,std::size_t,std::uint64_t,QPointer<NumericEngine>);
    void coalesce_preview_frames(const EndpointIdentity&,const FrameStamp&,Status);
    void queue_preview(ContextIterator,LatestIterator,const dataflow_runtime::NumericPortFrameView&);
    void queue_tap_frame(ContextIterator,LatestIterator,const dataflow_runtime::NumericPortFrameView&);
    DeliveredNumericHistoryRow retain_history_row(ContextIterator,const dataflow_runtime::NumericHistoryRowView&,
                                                  NumericHistoryEvent,DeliveryArena&);
    void queue_preview_row(ContextIterator,const dataflow_runtime::NumericHistoryRowView&,NumericHistoryEvent);
    void queue_transport_row(ContextIterator,const dataflow_runtime::NumericHistoryRowView&,NumericHistoryEvent);
    using PreviewSlots=std::array<std::optional<DeliveredNumericFrame>,128>;
    static void mark_latest_previews(const PreviewSlots&,std::array<bool,128>&,std::size_t count,std::size_t depth,
                                     const std::vector<EndpointIdentity>& subscriptions);
    void restore_previews(PreviewSlots&,const std::array<bool,128>&,std::size_t count);
    bool begin_preview_flush();
    void drain_preview_frames(QPointer<NumericEngine> lifetime);
    void drain_preview_rows(QPointer<NumericEngine> lifetime);
    void drain_frames(QPointer<NumericEngine> lifetime);
    void drain_rows(QPointer<NumericEngine> lifetime);
    struct WledSend {
        domain::NodeId node;
        std::shared_ptr<QUdpSocket> socket;
        QHostAddress destination,interface;
        quint16 port;
        wled_protocol::WledPacket packet;
    };
    std::size_t collect_due_wled(std::array<WledSend,512>& pending);
    void send_wled_packet(WledSend& output);
    void output(const dataflow_runtime::NumericPortFrameView& borrowed);
    void history(const dataflow_runtime::NumericHistoryRowView& borrowed);
    std::size_t preview_depth_locked() const;
    void normalize_previews_locked();
    void schedule_preview_locked();
    void dispatch_previews();
    void drain_previews();
    void drain();
    void flush_wled();
};
}
