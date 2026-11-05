# Explicit research composition only; no application or portable SDK dependency.
add_library(desktop_fourier_backend STATIC EXCLUDE_FROM_ALL
    kiss_backend.cpp
    ooura_backend.cpp
    vendor/kiss_fft130/kiss_fft.c
    vendor/kiss_fft130/tools/kiss_fftr.c
    vendor/aubio_ooura/ooura_fft8g.c)
set_target_properties(desktop_fourier_backend PROPERTIES
    AUTOMOC OFF C_STANDARD 99 CXX_STANDARD 20
    SPDX_LICENSE_IDENTIFIER "GPL-3.0-or-later")
target_include_directories(desktop_fourier_backend
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE vendor/kiss_fft130 vendor/kiss_fft130/tools vendor/aubio_ooura)
target_link_libraries(desktop_fourier_backend PUBLIC dsp_kernels)
# Keep the optional provider's C symbols private to this adapter's namespace.
target_compile_definitions(desktop_fourier_backend PRIVATE
    kiss_fft_alloc=barelief_kiss130_fft_alloc
    kiss_fft_stride=barelief_kiss130_fft_stride
    kiss_fft=barelief_kiss130_fft
    kiss_fft_cleanup=barelief_kiss130_fft_cleanup
    kiss_fft_next_fast_size=barelief_kiss130_fft_next_fast_size
    kiss_fftr_alloc=barelief_kiss130_fftr_alloc
    kiss_fftr=barelief_kiss130_fftr
    kiss_fftri=barelief_kiss130_fftri)
if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    target_compile_options(desktop_fourier_backend PRIVATE -fstack-usage)
endif()
