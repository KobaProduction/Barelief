#pragma once
#include "numeric_test_builders.h"
#include <domain/graph/admission.h>
#include <domain/signal/contracts.h>

namespace numeric_test_builders {
inline domain::ConfiguredProcessingNode real_fourier(
    domain::NodeId id, const signal_contracts::NumericContract& input,
    std::uint64_t size, const domain::ProcessingAdmissionLimits& limits, Check check) {
    using namespace domain;
    using namespace signal_contracts;
    ConfiguredProcessingNode result;
    result.id=id;result.kind="RealFourier";
    result.parameters=TransformProcessingParameters{{{"fft_size",NumericLiteral{size}}}};
    result.inputs={port(0,PortDirection::Input,input,limits.frame,limits.max_owned_metadata_bytes,check,"Fourier input")};
    std::vector<OwnedProcessingContract> outputs;
    check(fourier_output_contracts(result,input,outputs)==Status::ok,"derive real Fourier planes");
    for(std::size_t index=0;index<outputs.size();++index)
        result.outputs.push_back(port(PortId(index),PortDirection::Output,std::move(outputs[index])));
    return result;
}

inline domain::ConfiguredProcessingNode amplitude_phase(
    domain::NodeId id, const domain::ConfiguredProcessingNode& fourier,
    std::uint64_t size, std::uint32_t sample_rate,
    const domain::NumericArray& coefficients, Check check) {
    using namespace domain;
    using namespace signal_contracts;
    ConfiguredProcessingNode result;
    result.id=id;result.kind="AmplitudePhase";
    result.parameters=TransformProcessingParameters{{
        {"fft_size",NumericLiteral{size}},{"sample_rate",NumericLiteral{std::uint64_t{sample_rate}}},
        {"positive_half",true},{"representation",std::string{"peak_amplitude"}},
        {"normalize",true},{"window_coefficients",coefficients}}};
    for(const auto& plane:fourier.outputs)
        result.inputs.push_back(port(plane.id,PortDirection::Input,plane.contract));
    std::array<OwnedProcessingContract,3> outputs;
    const auto real=std::get<NumericContract>(fourier.outputs[0].contract.view());
    check(amplitude_phase_output_contracts(result,real,outputs)==Status::ok,"derive calibrated amplitude");
    for(std::size_t index=0;index<outputs.size();++index)
        result.outputs.push_back(port(PortId(index),PortDirection::Output,std::move(outputs[index])));
    return result;
}
}
