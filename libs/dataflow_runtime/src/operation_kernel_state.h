#pragma once
#include "music_execution.h"
#include "prepared_node.h"
#include <dsp_kernels/dsp.h>
#include <dsp_kernels/musical_features.h>
#include <dsp_kernels/frequency_profile.h>
namespace dataflow_runtime::detail {


struct OperationKernelState {
    std::variant<std::monostate,dsp_kernels::FFT,dsp_kernels::PreparedRfft,dsp_kernels::PreparedSpectrumProjection,
        dsp_kernels::BroadbandFeatures,dsp_kernels::NoteSalience,dsp_kernels::MusicalSpectrogram,dsp_kernels::SpectralQuantizer,dsp_kernels::PreparedFrequencyNormalizer> operation;
    std::vector<float> input,output;
    std::vector<float> imaginary;
    std::vector<float> factors;
    std::vector<std::uint8_t> factor_ready,factor_initial_ready;
    std::size_t frame_input=0;
    std::uint32_t note_first_midi=21;
    bool product_pending=false,add_arrays=false,clamp_floor=false;
    FrameStamp product_stamp{};
    std::vector<char> data_stream;std::size_t stream_size=0;
    std::vector<float> note_power;
    dsp_kernels::PreparedSpectrumScaling spectrum_scaling;
    dsp_kernels::PreparedSpectrumPower spectrum_power;
    double minimum=0,bin_hz=1;
    std::array<FrameStamp,2> stamps{};
    std::array<bool,2> pending{};
    std::vector<std::uint8_t> quantized8;std::vector<std::uint16_t> quantized16;
};
signal_contracts::Status prepare_numeric_operation(OperationKernelState&,PlanNode&,const domain::ResolvedProcessingNode&,const NumericRuntimeProfile&);
signal_contracts::Status prepare_signal_operation(OperationKernelState&,PlanNode&,const domain::ResolvedProcessingNode&);
signal_contracts::Status prepare_signal_features(OperationKernelState&,PlanNode&,State&,const domain::ResolvedProcessingNode&);
signal_contracts::Status prepare_music_operation(OperationKernelState&,PlanNode&,State&,const domain::ResolvedProcessingNode&);
signal_contracts::Status accept_array_product(OperationKernelState&,const PlanNode&,State&,std::size_t,const signal_contracts::DataFrameView&,const signal_contracts::FrameLimits&) noexcept;
signal_contracts::Status execute_musical_spectrogram(OperationKernelState&,const PlanNode&,State&,const signal_contracts::DataFrameView&) noexcept;
signal_contracts::Status process_prepared_operation(OperationKernelState&,bool discontinuity) noexcept;
std::string_view operation_text(const domain::ResolvedProcessingNode&,std::string_view) noexcept;
}
