// Explicit optional composition consumer. Building this executable verifies
// factory/runtime link closure; it is not a numerical or rhythm-quality test.
#include <rhythm_graph_adapters/profile.h>
#include <rhythm_graph_adapters/batch_composition.h>
#include <dsp_graph_adapters/spectral_composition.h>
#include <array>
#ifdef BARELIEF_PC_SINC_PROVIDER
#include "sinc_backend.h"
#endif

bool spectral_diagnostic_regressions();

namespace {
dataflow_runtime::NumericRuntimeProfile pc_profile() {
    dataflow_runtime::NumericRuntimeProfile profile{};
    profile.admission={64,128,64,128,4096,1024*1024,{8,1024*1024,256*1024*1024,1024*1024,4096,8*1024*1024}};
    profile.resources={{64*1024*1024,256*1024*1024,256*1024*1024,4*1024*1024},0,1024*1024*1024};
#ifdef BARELIEF_PC_SINC_PROVIDER
    auto provider=std::shared_ptr<const dsp_kernels::ResamplerBackendFactory>(
        &desktop_resampling::sinc_backend_factory(),[](const dsp_kernels::ResamplerBackendFactory*) noexcept {});
    return rhythm_graph_adapters::with_reference_operations(std::move(profile),{},std::move(provider));
#else
    return rhythm_graph_adapters::with_reference_operations(std::move(profile));
#endif
}
signal_contracts::Status lag_node(const domain::ProcessingAdmissionLimits& limits,domain::ConfiguredProcessingNode& node) {
    static constexpr std::uint32_t mono=0;signal_contracts::NumericContract input;
    input.dtype=signal_contracts::DType::Int32;input.channel_order={&mono,1};input.semantic=signal_contracts::Semantic::Numeric;
    input.shape={signal_contracts::Rank::Vector,{signal_contracts::Axis::Declared,signal_contracts::Axis::None},
        {signal_contracts::ExtentBounds{8,8},signal_contracts::ExtentBounds{}}};
    std::array<domain::OwnedProcessingContract,1> owned;
    const auto status=domain::own_processing_contract(input,limits.frame,limits.max_owned_metadata_bytes,owned[0]);
    if(status!=signal_contracts::Status::ok)return status;
    return rhythm_graph_adapters::make_batch_operation_node(0,"LagTempoConsensus",owned,{},limits.frame,limits.max_owned_metadata_bytes,node);
}
bool spectral_node(const dataflow_runtime::NumericRuntimeProfile& profile,std::string_view kind,signal_contracts::Rank rank,std::size_t a,std::size_t b) {
    static constexpr std::uint32_t mono=0;signal_contracts::NumericContract input;
    input.dtype=signal_contracts::DType::Float32;input.channel_order={&mono,1};input.unit=signal_contracts::NumericUnit::Values;
    input.shape={rank,{signal_contracts::Axis::Declared,rank==signal_contracts::Rank::Matrix?signal_contracts::Axis::Declared:signal_contracts::Axis::None},
        {signal_contracts::ExtentBounds{a,a},signal_contracts::ExtentBounds{b,b}}};
    domain::OwnedProcessingContract owned;
    if(domain::own_processing_contract(input,profile.admission.frame,profile.admission.max_owned_metadata_bytes,owned)!=signal_contracts::Status::ok)return false;
    const std::array<domain::ProcessingPort,1> ports{{{0,domain::PortDirection::Input,std::move(owned)}}};domain::ConfiguredProcessingNode node;
    if(dsp_graph_adapters::make_spectral_operation_node(1,kind,ports,{},profile.admission.frame,profile.admission.max_owned_metadata_bytes,node)!=signal_contracts::Status::ok)return false;
    const domain::ResolvedProcessingNode resolved{node.id,node.kind,domain::NodeRole::Processor,domain::ProcessingOperation::NumericExtension,node.inputs,node.outputs,&node.parameters,node.enabled};
    std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> descriptor;std::unique_ptr<dataflow_runtime::PreparedNumericOperation> operation;
    return profile.operation_factory->assess(resolved,profile).supported &&
        profile.operation_factory->describe(resolved,descriptor)==signal_contracts::Status::ok &&
        profile.operation_factory->prepare(std::move(descriptor),profile,operation)==signal_contracts::Status::ok;
}
}
int main() {
    if(!spectral_diagnostic_regressions())return 4;
    auto profile=pc_profile();domain::ConfiguredProcessingNode node;
    if(!spectral_node(profile,"SpectralNovelty",signal_contracts::Rank::Matrix,8,4) ||
       !spectral_node(profile,"ActivationPeakMatrix",signal_contracts::Rank::Matrix,8,4) ||
       !spectral_node(profile,"PcmSpectralNovelty",signal_contracts::Rank::Vector,4096,1))return 3;
    if(lag_node(profile.admission,node)!=signal_contracts::Status::ok)return 1;
    const domain::ResolvedProcessingNode operation{node.id,node.kind,domain::NodeRole::Processor,domain::ProcessingOperation::NumericExtension,node.inputs,node.outputs,&node.parameters,node.enabled};
    const auto supported=profile.operation_factory->assess(operation,profile).supported;
    dataflow_runtime::GraphExecutor executor{std::move(profile)};
    return supported?0:2;
}
