// SPDX-License-Identifier: AGPL-3.0-or-later
#include "pcm_stage_operations.h"
#include "pcm_stage_write.h"

namespace rhythm_graph_adapters::detail {
using namespace dataflow_runtime;
using namespace signal_contracts;
Status write_pcm_complex(NumericOperationOutput& output,std::span<const float> values,const FrameStamp& stamp) noexcept {
    if(values.size()%2)return Status::invalid_argument;
    output.value.actual={values.size()/2,2};
    for(std::size_t row=0;row<output.value.actual[0];++row)
        for(std::size_t column=0;column<2;++column){
            const auto status=store_element(output.value,{0,row,column},values[2*row+column]);if(status!=Status::ok)return status;
        }
    output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
Status PcmStageOperation::publish_spectrum(NumericOperationOutput& output,domain::PortId port,
    const rhythm_reference_ports::PreparedHopSpectrum& source,std::span<const float> magnitude,std::span<const float> phase,
    const FrameStamp& stamp) noexcept {
    if(port==0)return write_vector(output,magnitude,stamp);
    if(port==1)return write_vector(output,phase,stamp);
    if(port==2)return write_pcm_complex(output,source.complex_bins(),stamp);
    if(port==3)return write_vector(output,source.raw_pcm_history(),stamp);
    if(port==4)return write_vector(output,source.window_coefficients(),stamp);
    if(port==5)return write_vector(output,source.transform_stage().windowed_input,stamp);
    if(port==6)return write_vector(output,source.transform_stage().transform_input,stamp);
    if(port==7)return write_pcm_scalar(output,std::int64_t(samples_)-std::int64_t(plan_.onset.spectrum.window_samples),stamp);
    return Status::invalid_argument;
}
}
