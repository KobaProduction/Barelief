#pragma once
#include <pipeline/numeric_projection.h>
#include <map>
#include <visualization/numeric_matrix_snapshot.h>
class AbstractChartView;
class SpectrogramGraph;
class QLabel;
class NumericImageView;
class NumericPieView;
class NumericMetricsView;
namespace presentation {
struct NumericViewLimits {
    std::size_t max_bindings=0;
    pipeline::HistoryPresentationLimits history;
    pipeline::FloatProjectionPolicy vectors;
};
struct NumericRenderTarget {
    AbstractChartView* chart=nullptr;
    SpectrogramGraph* spectrogram=nullptr;
    QLabel* exact_label=nullptr;
    bool visible=false;
    NumericImageView* image=nullptr;
    NumericPieView* pie=nullptr;
    NumericMetricsView* metrics=nullptr;
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
        bool matrix_projected=false;
        std::size_t matrix_inexact_elements=0;
        pipeline::DeliveryStamp delivery_fence;
        bool fence_seen=false;
    };
    NumericViewLimits limits_;
    std::map<Key,State> states_;
};
}
