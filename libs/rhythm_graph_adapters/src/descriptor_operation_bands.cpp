// SPDX-License-Identifier: AGPL-3.0-or-later
#include "descriptor_operations.h"
#include "operation_io.h"

namespace rhythm_graph_adapters::detail {
using namespace signal_contracts;
namespace {
Status write_centers(dataflow_runtime::NumericOperationOutput& output,std::span<const double> centers,const FrameStamp& stamp) noexcept {
    output.value.actual={centers.size(),1};
    // Centers came from explicit Float32 source edges, so this restores their
    // original numeric representation; the payload axis remains band index.
    for(std::size_t i=0;i<centers.size();++i){
        const auto status=store_element(output.value,{0,i,0},float(centers[i]));if(status!=Status::ok)return status;
    }
    output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
Status write_weights(dataflow_runtime::NumericOperationOutput& output,std::span<const float> weights,
    std::size_t bands,std::size_t bins,const FrameStamp& stamp) noexcept {
    output.value.actual={bands,bins};
    for(std::size_t band=0;band<bands;++band)
        for(std::size_t bin=0;bin<bins;++bin){
            const auto status=store_element(output.value,{0,band,bin},weights[band*bins+bin]);if(status!=Status::ok)return status;
        }
    output.stamp=stamp;output.status=Status::ok;return Status::ok;
}
}
Status DescriptorOperation::publish_bands(std::span<dataflow_runtime::NumericOperationOutput> outputs,const FrameStamp& stamp) noexcept {
    const auto& kernel=std::get<rhythm_reference_ports::PreparedTriangularSpectralBands>(kernel_);
    auto status=write_vector(*output_at(outputs,0),values_,stamp);if(status!=Status::ok)return status;
    status=write_centers(*output_at(outputs,1),kernel.centers_hz(),stamp);if(status!=Status::ok)return status;
    return write_weights(*output_at(outputs,2),kernel.weights(),plan_.bands,plan_.bins,stamp);
}
}
