// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdlib>
#include <limits>
#ifdef _WIN32
#include <malloc.h>
#endif

namespace test_allocation {
// Aligned new accepts arbitrary byte counts; aligned_alloc requires a multiple.
[[nodiscard]] inline void* allocate_aligned(std::size_t bytes, std::size_t alignment) noexcept {
    if (!alignment || (alignment & (alignment - 1))) return nullptr;
    const auto size = bytes ? bytes : 1;
    if (size > std::numeric_limits<std::size_t>::max() - (alignment - 1)) return nullptr;
    const auto rounded = (size + alignment - 1) / alignment * alignment;
#ifdef _WIN32
    return _aligned_malloc(rounded, alignment);
#else
    return std::aligned_alloc(alignment, rounded);
#endif
}
inline void deallocate_aligned(void* pointer) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
}
