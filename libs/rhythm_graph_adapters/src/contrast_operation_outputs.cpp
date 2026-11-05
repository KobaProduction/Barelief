// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status write_counter(NumericOperationOutput& output,std::uint64_t value,const FrameStamp& stamp) noexcept {
    output.value.actual={1,1};const auto status=store_element(output.value,{0,0,0},value);
    if(status==Status::ok){output.stamp=stamp;output.status=Status::ok;}return status;
}
FrameStamp event_stamp(const FrameStamp& source) noexcept {
    return batch_stamp(source);
}
}
Status ContrastOperation::publish(std::span<NumericOperationOutput> outputs,NumericOperationEvent event) noexcept {
    if(plan_.accumulator){
        const auto events=event==NumericOperationEvent::EndOfStream?std::span<const float>(final_events_).first(count_):
            std::get<PreparedContrastPeakAccumulator>(kernel_).onsets();
        const auto status=write_vector(*output_at(outputs,0),events,event_stamp(source_stamp_));if(status!=Status::ok)return status;
        return write_counter(*output_at(outputs,1),std::uint64_t(count_),source_stamp_);
    }
    const auto& kernel=std::get<PreparedContrastPeakPicker>(kernel_);
    auto status=write_vector(*output_at(outputs,0),kernel.onsets(),event_stamp(source_stamp_));if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,1),kernel.averages(rows_),source_stamp_);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,2),kernel.maxima(rows_),source_stamp_);if(status!=Status::ok)return status;
    return write_counter(*output_at(outputs,3),kernel.detections(),source_stamp_);
}
}
