#pragma once
#include <cstddef>
int fft4_dispatch(float*, std::size_t);
int reverse4_dispatch(float*, std::size_t);
#define dsps_fft4r_fc32 fft4_dispatch
#define dsps_bit_rev4r_fc32 reverse4_dispatch
