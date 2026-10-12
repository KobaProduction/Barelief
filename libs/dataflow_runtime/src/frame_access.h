#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
MutableNumericView mutable_output(const PlanNode&,State&,PortId) noexcept;
std::variant<NumericView,StructuredView> zero_value(const DataContract&,const NumericBuffer&) noexcept;
}
