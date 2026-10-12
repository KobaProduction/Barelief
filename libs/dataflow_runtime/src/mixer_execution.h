#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
struct MixerDelivery {
    void* user=nullptr;
    Status (*send)(void*,const DataFrameView&,Status)=nullptr;
    std::uint64_t& child_nanoseconds;
};
bool aligned_operation(const PlanNode&,const NumericProcessingGraph&) noexcept;
Status accept_mixer_contribution(const PlanNode&,State&,std::size_t,const DataFrameView&,const NumericProcessingGraph&,const NumericRuntimeProfile&,MixerDelivery) noexcept;
}
