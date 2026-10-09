#pragma once
#include <signal_contracts/data_frame.h>
#include <signal_contracts/numeric_policy.h>
#include <array>
#include <limits>

namespace dsp_kernels {
enum class NumericStatus { Ok, UnsupportedCombination, InvalidContract, InvalidPolicy, InvalidLayout,
    EmptyPopulation, LimitExceeded, BufferTooSmall, NonFiniteInput, Overflow, PrecisionLoss, BackendFailure };
enum class LossFlag : std::uint32_t { None=0, Rounded=1, Saturated=2, ReplacedNonFinite=4, Underflow=8 };
struct NumericResult {
    NumericStatus status;
    std::size_t used_scratch=0, required_scratch=0;
    std::size_t error_plane=std::numeric_limits<std::size_t>::max(), error_element=std::numeric_limits<std::size_t>::max();
    std::uint32_t loss_flags=0;
};
struct KernelLimits { std::size_t max_elements, max_planes, max_input_bytes, max_output_bytes, max_scratch_bytes; };
struct KernelRequirements { std::size_t scratch_bytes=0, scratch_alignment=1, plan_bytes=0, stack_bytes=0; };
// Conservative reference stack reservation. Compiler/target integration must
// verify its call chain (including libc and contract adapters); not an ESP ABI measurement.
inline constexpr std::size_t numeric_reference_stack_bytes=8192;
struct KernelCapabilities { bool converter, reduction, float64_arithmetic; };
enum class KernelKind { Converter, Reduction, WindowCoefficients, FFT, Bark, Mel, StereoProcessor };
struct KernelSupport { KernelKind kernel; signal_contracts::DType dtype; signal_contracts::ValueDomain input_values;
    std::size_t maximum_fft_samples; bool optimized; };
// Source dtype inventory. Detailed pair/policy/representation admission is the
// prepare/support query; rows alone never authorize a dtype pair or History.
// optimized=false means no target-specific acceleration is promised here.
std::span<const KernelSupport> kernel_support_table() noexcept;
// Compiled library capability for Binary64Affine and floating Mean/RMS.
// Independent of software-bit Float64 conversion/exact integer aggregation;
// unavailable native environment refuses only these modes at Prepare.
bool declared_ieee64_environment_supported() noexcept;
// Fixed bounded immutable snapshots; contract() rebases spans after every copy/move.
// These are kernel admission ceilings, not a replacement dtype/domain registry.
struct KernelContract {
    static constexpr std::size_t max_planes=64, max_clock_bytes=256;
    signal_contracts::NumericContract value;
    std::array<std::uint32_t,max_planes> channels{};
    std::array<char,max_clock_bytes> clock{};
    std::size_t plane_count=0, clock_size=0;
    signal_contracts::NumericContract contract() const noexcept {
        auto out=value;out.channel_order={channels.data(),plane_count};out.clock.domain={clock.data(),clock_size};return out;
    }
};
}
