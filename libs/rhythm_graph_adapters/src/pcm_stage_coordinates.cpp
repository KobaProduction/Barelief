// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include <cmath>

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
namespace {
Status write_axis(NumericOperationOutput& output,const PcmStagePlan& plan,PcmStageAxis axis,double first) noexcept {
    if(axis==PcmStageAxis::Index)return Status::ok;
    for(std::size_t i=0;i<output.value.actual[0];++i){
        const double coordinate=axis==PcmStageAxis::Frequency?
            double(i)*double(plan.sample_rate)/double(plan.onset.spectrum.window_samples):(first+double(i))/double(plan.sample_rate);
        const auto status=numeric_operation_coordinate(output,0,i,coordinate);if(status!=Status::ok)return status;
    }
    return numeric_operation_axis(output,0,output.value.actual[0]);
}
}
Status PcmStageOperation::prepare_output_views(std::span<NumericOperationOutput> outputs,bool next_frame) const noexcept {
    const double first=double(samples_)+double(next_frame?plan_.onset.spectrum.hop_samples:0)-double(plan_.onset.spectrum.window_samples);
    for(auto& output:outputs){
        const auto spec=pcm_stage_output_spec(plan_,output.port);
        output.value.actual={spec.variable?0:spec.rows,spec.columns};output.value.coordinates={};
        auto status=write_axis(output,plan_,spec.axis,first);if(status!=Status::ok)return status;
        status=validate_mutable(output.value,limits_);if(status!=Status::ok)return status;
    }
    return Status::ok;
}
}
