#include "operation_kernel_state.h"
#include <domain/music/contracts.h>

namespace dataflow_runtime::detail {
signal_contracts::Status prepare_spectral_descriptor(OperationKernelState& kernel,const domain::ResolvedProcessingNode& node) {
    using namespace dsp_kernels;using signal_contracts::Status;
    domain::SpectralDescriptorParameters p;
    if(!domain::spectral_descriptor_parameters(node,p))return Status::invalid_argument;
    SpectralDescriptorConfig config{SpectralDescriptor::Flux,p.half_wave,p.weighted,p.squared,p.magnitude_threshold};
    if(node.kind=="ComplexSpectralDifference")config.method=SpectralDescriptor::ComplexDifference;
    if(node.kind=="PhaseDeviation")config.method=SpectralDescriptor::PhaseDeviation;
    if(node.kind=="HighFrequencyContent")config.method=SpectralDescriptor::HighFrequencyContent;
    auto& operation=kernel.operation.emplace<PreparedSpectralDescriptor>();
    return operation.prepare(kernel.input.size()/2,config)?Status::ok:Status::invalid_argument;
}
}
