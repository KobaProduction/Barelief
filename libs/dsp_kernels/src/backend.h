#pragma once
#include <cstddef>
namespace dsp_kernels::detail {
bool prepare_backend(std::size_t samples, bool radix4);
bool transform(float *scratch, std::size_t samples, bool radix4);
std::size_t shared_table_bytes(bool radix4);
}
