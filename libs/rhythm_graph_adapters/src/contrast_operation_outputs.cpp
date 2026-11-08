// SPDX-License-Identifier: AGPL-3.0-or-later
#include "contrast_operations.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace rhythm_reference_ports;
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status write_counter(NumericOperationOutput& output,std::uint64_t value,const dataflow_runtime::NumericOperationOrigin& stamp) noexcept {
    output.value.actual={1,1};const auto status=store_element(output.value,{0,0,0},value);
    if(status==Status::ok){output.origin=stamp;output.status=Status::ok;}return status;
}
dataflow_runtime::NumericOperationOrigin event_stamp(const dataflow_runtime::NumericOperationOrigin& source) noexcept {
    return batch_stamp(source);
}
}
namespace {
Status publish_picker(std::span<NumericOperationOutput> outputs,const PreparedContrastPeakPicker& kernel,std::size_t rows,
    const NumericOperationOrigin& source) noexcept {
    auto status=write_vector(*output_at(outputs,0),kernel.onsets(),event_stamp(source));if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,1),kernel.averages(rows),source);if(status!=Status::ok)return status;
    status=write_vector(*output_at(outputs,2),kernel.maxima(rows),source);if(status!=Status::ok)return status;
    return write_counter(*output_at(outputs,3),kernel.detections(),source);
}
}
Status ContrastOperation::publish(std::span<NumericOperationOutput> outputs,NumericOperationEvent event) noexcept {
    if(!plan_.accumulator)return publish_picker(outputs,std::get<PreparedContrastPeakPicker>(kernel_),rows_,source_stamp_);
    const auto events=event==NumericOperationEvent::EndOfStream?std::span<const float>(final_events_).first(count_):
        std::get<PreparedContrastPeakAccumulator>(kernel_).onsets();
    const auto status=write_vector(*output_at(outputs,0),events,event_stamp(source_stamp_));if(status!=Status::ok)return status;
    return write_counter(*output_at(outputs,1),std::uint64_t(count_),source_stamp_);
}
}
