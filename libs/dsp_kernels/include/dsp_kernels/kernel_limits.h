#pragma once
#include <cstddef>
namespace dsp_kernels {
struct KernelLimits {std::size_t max_elements,max_planes,max_input_bytes,max_output_bytes,max_scratch_bytes;};
struct KernelRequirements {std::size_t scratch_bytes=0,scratch_alignment=1,plan_bytes=0,stack_bytes=0;};
struct KernelCapabilities {bool converter,reduction,float64_arithmetic;};
}
