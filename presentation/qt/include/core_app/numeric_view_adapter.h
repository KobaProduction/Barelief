#pragma once
#include <pipeline/numeric_projection.h>
#include <map>
#include <optional>
#include <barelief/numeric_matrix_snapshot.h>
#include <barelief/quick/numeric_render_ports.h>
namespace presentation {
struct NumericViewLimits {
    std::size_t max_bindings=0;
    pipeline::HistoryPresentationLimits history;
    pipeline::FloatProjectionPolicy vectors;
};
// GUI event-loop adapter. Binding is explicit: Sink Input or Source Output;
// panel/device identity and workspace preferences remain the composition's concern.
class NumericViewAdapter {
public:
    explicit NumericViewAdapter(NumericViewLimits limits):limits_(limits){}
    signal_contracts::Status bind(const pipeline::EndpointIdentity&,std::size_t plane=0);
    void unbind(const pipeline::EndpointIdentity&,std::size_t plane=0);
    signal_contracts::Status accept(const pipeline::DeliveredNumericFrame&);
    signal_contracts::Status accept(const pipeline::DeliveredNumericHistoryRow&);
    signal_contracts::Status accept_history_snapshot(const pipeline::DeliveredNumericFrame&,std::uint64_t ordinal,std::uint64_t filled_count);
    signal_contracts::Status render(const pipeline::EndpointIdentity&,std::size_t plane,NumericRenderTarget);
    void clear() noexcept;
    static void clear_render_target(NumericRenderTarget);
private:
    struct Key {
        pipeline::EndpointIdentity endpoint;std::size_t plane=0;
        bool operator<(const Key& other) const {return std::tie(endpoint,plane)<std::tie(other.endpoint,other.plane);}
    };
    struct State {
        explicit State(pipeline::HistoryPresentationLimits limits):history(limits){}
        // Contract identity survives temporary unavailability, when no frame
        // owner is retained. A later incompatible stream must reset its ring.
        std::shared_ptr<const pipeline::NumericDeliveryContext> context;
        pipeline::DeliveredNumericFrame latest;
        pipeline::HistoryPresentation history;
        signal_contracts::Status status=signal_contracts::Status::unavailable;
        bool history_mode=false;
        NumericMatrixSnapshot matrix_snapshot;
        NumericMatrixSnapshotPool matrix_pool;
        bool matrix_projected=false;
        std::size_t matrix_inexact_elements=0;
        pipeline::DeliveryStamp delivery_fence;
        bool fence_seen=false;
    };
    std::optional<signal_contracts::Status> accept_frame_for_state(const Key&,State&,
        const pipeline::DeliveredNumericFrame&);
    std::optional<signal_contracts::Status> accept_available_frame(const Key&,State&,
        const pipeline::DeliveredNumericFrame&,bool changed);
    bool reset_frame_state(State&,const pipeline::DeliveredNumericFrame&);
    bool stale_frame_sequence(const State&,const pipeline::DeliveredNumericFrame&,bool changed) const;
    signal_contracts::Status validate_scalar_frame(const Key&,State&,const signal_contracts::NumericView&);
    signal_contracts::Status accept_snapshot_for_state(const Key&,State&,
        const pipeline::DeliveredNumericFrame&,std::uint64_t ordinal,std::uint64_t filled_count);
    signal_contracts::Status render_history(State&,NumericRenderTarget);
    signal_contracts::Status publish_history_snapshot(State&,NumericRenderTarget);
    SpectrogramAxisMetadata history_axes(const State&,NumericRenderTarget) const;
    void normalize_history_axes(SpectrogramAxisMetadata&,NumericRenderTarget) const;
    void prepare_history_snapshot(State&);
    void history_captions(const State&,NumericRenderTarget) const;
    void prepare_matrix_snapshot(State&,std::size_t,std::size_t,const std::vector<float>&,const pipeline::FloatProjectionReport&);
    signal_contracts::Status render_frame(State&,const pipeline::EndpointIdentity&,std::size_t,NumericRenderTarget);
    signal_contracts::Status render_numeric_frame(State&,const pipeline::EndpointIdentity&,std::size_t,
        const signal_contracts::NumericView&,NumericRenderTarget);
    struct FrameProjection {std::vector<float> values;pipeline::FloatProjectionReport report;QString exact;};
    signal_contracts::Status project_frame(State&,const signal_contracts::NumericView&,std::size_t,FrameProjection&);
    signal_contracts::Status publish_frame(State&,const signal_contracts::NumericView&,FrameProjection&,NumericRenderTarget);
    signal_contracts::Status configure_vector(const signal_contracts::NumericView&,std::size_t,NumericRenderTarget);
    signal_contracts::Status vector_coordinates(const signal_contracts::NumericView&,NumericRenderTarget,
        std::vector<double>&,QString&);
    signal_contracts::Status render_matrix(State&,const signal_contracts::NumericView&,const std::vector<float>&,const pipeline::FloatProjectionReport&,NumericRenderTarget);
    NumericViewLimits limits_;
    std::map<Key,State> states_;
};
}
