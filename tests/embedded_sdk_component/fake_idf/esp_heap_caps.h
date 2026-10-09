#pragma once
#include <cstddef>
inline constexpr unsigned MALLOC_CAP_8BIT=4;
std::size_t heap_caps_get_free_size(unsigned);
