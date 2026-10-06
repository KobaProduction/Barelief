add_library(dsp STATIC)

target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/common/include_sim>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/fft/include>)

target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/blackman/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/blackman_harris/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/blackman_nuttall/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/flat_top/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/hann/include>)
target_include_directories(dsp PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/windows/nuttall/include>)

target_sources(dsp PRIVATE
        common/misc/dsps_pwroftwo.cpp
        fft/float/dsps_fft2r_fc32_ansi.c
        fft/float/dsps_fft4r_fc32_ansi.c
        fft/float/dsps_fft2r_bitrev_tables_fc32.c
        fft/float/dsps_fft4r_bitrev_tables_fc32.c

        fft/float/dsps_fht2r_fc32_ansi.c
        fft/float/dsps_fht4r_fc32_ansi.c

        windows/hann/float/dsps_wind_hann_f32.c
        windows/blackman/float/dsps_wind_blackman_f32.c
        windows/blackman_harris/float/dsps_wind_blackman_harris_f32.c
        windows/blackman_nuttall/float/dsps_wind_blackman_nuttall_f32.c
        windows/nuttall/float/dsps_wind_nuttall_f32.c
        windows/flat_top/float/dsps_wind_flat_top_f32.c
)

install(TARGETS dsp EXPORT portable_sdk_targets ARCHIVE DESTINATION lib)
