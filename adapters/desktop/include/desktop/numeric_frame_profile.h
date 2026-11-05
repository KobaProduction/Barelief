#pragma once
#include <signal_contracts/data_contract.h>
#include <desktop/numeric_frame_budget.h>

namespace desktop::numeric_profile {
// Coordinate bytes belong to the complete metadata ceiling in both execution
// and transport admission. These are bounds, not per-frame allocations.
inline constexpr std::size_t history_coordinate_bytes=4096*sizeof(double);
inline constexpr signal_contracts::FrameLimits frame_limits{
    8,NumericFrameBudget::elements,NumericFrameBudget::payload_bytes,NumericFrameBudget::metadata_bytes,256,NumericFrameBudget::coordinate_bytes};
}
