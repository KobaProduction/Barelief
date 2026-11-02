#pragma once
#include <cstddef>
#include <limits>

namespace dsp_kernels::prepared_storage {
// Checked prepared allocation payload arithmetic. No allocator/object/stack or
// process-wide table bytes are implicit; each caller declares those separately.
inline bool add_bytes(std::size_t& total,std::size_t value) noexcept {
    if(value>std::numeric_limits<std::size_t>::max()-total)return false;
    total+=value;return true;
}
inline bool mul_bytes(std::size_t count,std::size_t width,std::size_t& output) noexcept {
    if(width && count>std::numeric_limits<std::size_t>::max()/width)return false;
    output=count*width;return true;
}
inline bool add_array(std::size_t& total,std::size_t count,std::size_t width) noexcept {
    std::size_t value=0;return mul_bytes(count,width,value) && add_bytes(total,value);
}
} // namespace dsp_kernels::prepared_storage
