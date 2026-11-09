// Host composition consumer checks factory/runtime link closure and diagnostic
// lifecycles. Desktop test builds register it as a quick CTest.
#include <rhythm_graph_adapters/profile.h>
#include <rhythm_graph_adapters/batch_composition.h>
#include <dsp_graph_adapters/spectral_composition.h>
#include <array>
#include <vector>
#include <iostream>
#include <dsp_kernels/spectral_novelty.h>
#include <dataflow_runtime/numeric_operation_binding.h>
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
bool composition_check(bool success,std::string_view stage,std::string_view kind) {
    if(!success)std::cerr<<"Composition check failed: "<<kind<<": "<<stage<<'\n';
    return success;
}

bool prepare_operation(const dataflow_runtime::NumericRuntimeProfile& profile,const domain::ConfiguredProcessingNode& node) {
    const auto binding=dataflow_runtime::numeric_operation_node(node);
    const auto operation_profile=dataflow_runtime::numeric_operation_profile(profile);
    std::shared_ptr<const dataflow_runtime::NumericOperationDescriptor> descriptor;
    std::unique_ptr<dataflow_runtime::PreparedNumericOperation> operation;
    if(!composition_check(profile.operation_factory->assess(binding,operation_profile).supported,"assess",node.kind))return false;
    if(!composition_check(profile.operation_factory->describe(binding,descriptor)==signal_contracts::Status::ok,"describe",node.kind))return false;
    return composition_check(profile.operation_factory->prepare(std::move(descriptor),operation_profile,operation)==signal_contracts::Status::ok,"prepare",node.kind);
}
signal_contracts::NumericContract spectral_input(signal_contracts::Rank rank,std::size_t a,std::size_t b) {
    static constexpr std::uint32_t mono=0;
    signal_contracts::NumericContract input;
    input.dtype=signal_contracts::DType::Float32;
    input.channel_order={&mono,1};
    input.unit=signal_contracts::NumericUnit::Values;
    input.shape={rank,{signal_contracts::Axis::Declared,rank==signal_contracts::Rank::Matrix?signal_contracts::Axis::Declared:signal_contracts::Axis::None},
        {signal_contracts::ExtentBounds{a,a},signal_contracts::ExtentBounds{b,b}}};
    return input;
}

bool spectral_ports(const dataflow_runtime::NumericRuntimeProfile& profile,dsp_kernels::SpectralNoveltyMethod method,std::vector<domain::ProcessingPort>& ports) {
    const auto input=spectral_input(signal_contracts::Rank::Matrix,8,4);
    domain::OwnedProcessingContract owned;
    if(domain::own_processing_contract(input,profile.admission.frame,profile.admission.max_owned_metadata_bytes,owned)!=signal_contracts::Status::ok)return false;
    ports.push_back({0,domain::PortDirection::Input,owned});
    if(dsp_kernels::spectral_novelty_uses_phase(method))ports.push_back({1,domain::PortDirection::Input,std::move(owned)});
    return true;
}

bool spectral_node(const dataflow_runtime::NumericRuntimeProfile& profile,dsp_kernels::SpectralNoveltyMethod method) {
    std::vector<domain::ProcessingPort> ports;
    if(!composition_check(spectral_ports(profile,method,ports),"input contracts","SpectralNovelty"))return false;
    domain::TransformProcessingParameters parameters;
    parameters.values.push_back({"method",signal_contracts::NumericLiteral{std::uint64_t(method)}});
    domain::ConfiguredProcessingNode node;
    const auto status=dsp_graph_adapters::make_spectral_operation_node(1,"SpectralNovelty",ports,std::move(parameters),profile.admission.frame,profile.admission.max_owned_metadata_bytes,node);
    return composition_check(status==signal_contracts::Status::ok,"compose","SpectralNovelty") && prepare_operation(profile,node);
}
}
int main() {
    if(!composition_check(spectral_diagnostic_regressions(),"diagnostic lifecycle","SpectralNovelty"))return 4;
    auto profile=pc_profile();
    domain::ConfiguredProcessingNode node;
    if(!spectral_node(profile,dsp_kernels::SpectralNoveltyMethod::PositiveDifference) ||
       !spectral_node(profile,dsp_kernels::SpectralNoveltyMethod::PhaseMaskedDifference))return 3;
    if(lag_node(profile.admission,node)!=signal_contracts::Status::ok)return 1;
    return prepare_operation(profile,node)?0:2;
}
