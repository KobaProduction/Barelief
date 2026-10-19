#include "operation_kernel_state.h"
#include <cmath>
#include <limits>

namespace dataflow_runtime::detail {
using signal_contracts::Status;
namespace {
bool prepare_note_power(OperationKernelState& state) noexcept {
    return state.spectrum_power.process(state.input,state.note_power);
}
struct Process {
    OperationKernelState& state;
    bool discontinuity;
    bool operator()(std::monostate&) const noexcept {return false;}
    bool operator()(dsp_kernels::FFT&) const noexcept {return false;}
    bool operator()(dsp_kernels::PreparedRfft& operation) const noexcept {return operation.process(state.input,state.output);}
    bool operator()(dsp_kernels::PreparedSpectrumProjection& operation) const noexcept {return operation.process(state.input,state.output);}
    bool operator()(dsp_kernels::BroadbandFeatures& operation) const noexcept {return operation.process(state.input,state.output,discontinuity);}
    bool operator()(dsp_kernels::PreparedFrequencyNormalizer& operation) const noexcept {return operation.process(state.input,state.output);}
    bool operator()(dsp_kernels::MusicalSpectrogram&) const noexcept {return false;}
    bool operator()(dsp_kernels::SpectralQuantizer&) const noexcept {return true;}
    bool operator()(dsp_kernels::NoteSalience& operation) const noexcept {
        return prepare_note_power(state) && operation.process(state.note_power,state.output);
    }
};
}
Status process_prepared_operation(OperationKernelState& state,bool discontinuity) noexcept {
    return std::visit(Process{state,discontinuity},state.operation)?Status::ok:Status::overflow;
}
}
