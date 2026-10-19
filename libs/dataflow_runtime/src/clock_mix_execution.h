#pragma once
#include "node_state.h"
namespace dataflow_runtime::detail {
struct ClockMixResult {Status status=Status::ok;bool fatal=false;};
ClockMixResult mix_clock_frame(const PlanNode&,State&,const MixerProcessingParameters&,FrameStamp&,const FrameStamp&,std::size_t,bool,bool) noexcept;
}
