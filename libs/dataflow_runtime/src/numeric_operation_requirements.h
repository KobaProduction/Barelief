#pragma once
#include "numeric_operation_bridge.h"

namespace dataflow_runtime::detail {
struct NumericMetadataGrid {double origin=0,step=1,tolerance=0;bool any_origin=false;};
bool metadata_contract(const domain::ResolvedProcessingNode&,domain::PortDirection,domain::PortId,signal_contracts::NumericContract&) noexcept;
bool metadata_value(const NumericOperationInvocation&,domain::PortDirection,domain::PortId,signal_contracts::NumericView&,bool&) noexcept;
bool metadata_axis_valid(const signal_contracts::NumericContract&,const NumericOperationAxisRule&) noexcept;
bool metadata_axis_ignored(const signal_contracts::AxisCoordinates&,const NumericOperationAxisRule&) noexcept;
bool metadata_index_unit(signal_contracts::NumericUnit) noexcept;
bool metadata_grid(const signal_contracts::AxisCoordinates&,const NumericOperationAxisRule&,NumericMetadataGrid&) noexcept;
bool metadata_close(double,double,double) noexcept;
bool metadata_coordinate(std::span<const std::byte>,std::size_t,double&) noexcept;
bool metadata_axis_payload(const signal_contracts::NumericView&,std::size_t) noexcept;
bool metadata_rule_values(const signal_contracts::NumericView&,const NumericOperationAxisRule&) noexcept;
}
