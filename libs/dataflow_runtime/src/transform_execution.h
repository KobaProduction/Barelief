#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
// Calls remain synchronous: borrowed output storage is stable through descendants.
struct TransformDelivery {
    void* user;
    void (*emit)(void*);
    Status (*reject)(void*,Status);
};
struct TransformResult {Status status=Status::ok;bool completed=false;};
TransformResult execute_transform(const PlanNode&,State&,const DataFrameView&,const FrameLimits&,TransformDelivery) noexcept;
}
