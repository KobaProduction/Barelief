#pragma once
#include "numeric_engine_execution.h"
#include <QUdpSocket>
#include <wled/numeric_wled_output_policy.h>
#include <array>
#include <atomic>

namespace pipeline::numeric_engine_detail {
struct DeliveryArena {
    static constexpr std::size_t slot_count=128,payload_bytes=262144,metadata_bytes=16384;
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
    std::shared_ptr<const void> retention_arenas=std::make_shared<std::pair<std::shared_ptr<DeliveryArena>,std::shared_ptr<DeliveryArena>>>(arena,preview_arena);
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
    void commit(Staged&&);
    void clear();
    dataflow_runtime::NumericRuntimeCallbacks callbacks() noexcept;
    bool tap_selected(const EndpointIdentity& endpoint) const;
    bool selected(const EndpointIdentity& endpoint) const;
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
