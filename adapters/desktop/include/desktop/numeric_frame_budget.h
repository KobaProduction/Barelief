#pragma once
#include <cstddef>
#include <limits>

namespace desktop {
// Desktop sizes are user selected. These are representation limits, not quotas.
struct NumericFrameBudget {
    static constexpr std::size_t elements=std::numeric_limits<std::ptrdiff_t>::max();
    static constexpr std::size_t payload_bytes=elements,metadata_bytes=elements,coordinate_bytes=elements;
};
}
