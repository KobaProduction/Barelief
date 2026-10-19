#pragma once
#include <cstddef>
inline constexpr int ESP_OK = 0;
int fft2_dispatch(float*, std::size_t);
int reverse2_dispatch(float*, std::size_t);
#define dsps_fft2r_fc32 fft2_dispatch
#define dsps_bit_rev2r_fc32 reverse2_dispatch
